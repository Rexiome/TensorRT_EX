#include "NvInfer.h"
#include "cuda_runtime_api.h"
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <vector>
#include <chrono>
#include <algorithm>
#include "opencv2/opencv.hpp"
#include <string>
#include <io.h>				//access
#include "utils.hpp"		// custom function
#include "preprocess.hpp"	// preprocess plugin 
#include "logging.hpp"	

REGISTER_TENSORRT_PLUGIN(PreprocessPluginV2Creator);

// stuff we know about the network and the input/output blobs
static const int INPUT_H = 512;
static const int INPUT_W = 512;
static const int OUTPUT_SIZE = INPUT_H* INPUT_W * 2;
static const int INPUT_C = 3;
static const int precision_mode = 32; // fp32 : 32, fp16 : 16, int8(ptq) : 8

const char* INPUT_BLOB_NAME = "data";
const char* OUTPUT_BLOB_NAME = "prob";

using namespace nvinfer1;

static Logger gLogger;

// Load weights from files shared with TensorRT samples.
// TensorRT weight files have a simple space delimited format:
// [type] [size] <data x size in hex>
std::map<std::string, Weights> loadWeights(const std::string file)
{
	std::cout << "Loading weights: " << file << std::endl;
	std::map<std::string, Weights> weightMap;

	// Open weights file
	std::ifstream input(file);
	assert(input.is_open() && "Unable to load weight file.");

	// Read number of weight blobs
	int32_t count;
	input >> count;
	assert(count > 0 && "Invalid weight map file.");

	while (count--)
	{
		Weights wt{ DataType::kFLOAT, nullptr, 0 };
		uint32_t size;

		// Read name and type of blob
		std::string name;
		input >> name >> std::dec >> size;
		wt.type = DataType::kFLOAT;

		// Load blob
		uint32_t* val = reinterpret_cast<uint32_t*>(malloc(sizeof(val) * size));
		for (uint32_t x = 0, y = size; x < y; ++x)
		{
			input >> std::hex >> val[x];
		}
		wt.values = val;

		wt.count = size;
		weightMap[name] = wt;
	}

	return weightMap;
}

IScaleLayer* addBatchNorm2d(INetworkDefinition *network, std::map<std::string, Weights>& weightMap, ITensor& input, std::string lname, float eps) {
	float *gamma = (float*)weightMap[lname + ".weight"].values;
	float *beta = (float*)weightMap[lname + ".bias"].values;
	float *mean = (float*)weightMap[lname + ".running_mean"].values;
	float *var = (float*)weightMap[lname + ".running_var"].values;
	int len = weightMap[lname + ".running_var"].count;

	float *scval = reinterpret_cast<float*>(malloc(sizeof(float) * len));
	for (int i = 0; i < len; i++) {
		scval[i] = gamma[i] / sqrt(var[i] + eps);
	}
	Weights scale{ DataType::kFLOAT, scval, len };

	float *shval = reinterpret_cast<float*>(malloc(sizeof(float) * len));
	for (int i = 0; i < len; i++) {
		shval[i] = beta[i] - mean[i] * gamma[i] / sqrt(var[i] + eps);
	}
	Weights shift{ DataType::kFLOAT, shval, len };

	float *pval = reinterpret_cast<float*>(malloc(sizeof(float) * len));
	for (int i = 0; i < len; i++) {
		pval[i] = 1.0;
	}
	Weights power{ DataType::kFLOAT, pval, len };

	weightMap[lname + ".scale"] = scale;
	weightMap[lname + ".shift"] = shift;
	weightMap[lname + ".power"] = power;
	IScaleLayer* scale_1 = network->addScale(input, ScaleMode::kCHANNEL, shift, scale, power);
	assert(scale_1);
	return scale_1;
}

ILayer* doubleConv(INetworkDefinition *network, std::map<std::string, Weights>& weightMap, ITensor& input, int outch, int ksize, std::string lname, int midch) {

	IConvolutionLayer* conv1 = network->addConvolutionNd(input, midch, DimsHW{ ksize, ksize }, weightMap[lname + ".double_conv.0.weight"], weightMap[lname + ".double_conv.0.bias"]);
	conv1->setStrideNd(DimsHW{ 1, 1 });
	conv1->setPaddingNd(DimsHW{ 1, 1 });
	conv1->setNbGroups(1);
	IScaleLayer* bn1 = addBatchNorm2d(network, weightMap, *conv1->getOutput(0), lname + ".double_conv.1", 0);
	IActivationLayer* relu1 = network->addActivation(*bn1->getOutput(0), ActivationType::kLEAKY_RELU);
	IConvolutionLayer* conv2 = network->addConvolutionNd(*relu1->getOutput(0), outch, DimsHW{ 3, 3 }, weightMap[lname + ".double_conv.3.weight"], weightMap[lname + ".double_conv.3.bias"]);
	conv2->setStrideNd(DimsHW{ 1, 1 });
	conv2->setPaddingNd(DimsHW{ 1, 1 });
	conv2->setNbGroups(1);
	IScaleLayer* bn2 = addBatchNorm2d(network, weightMap, *conv2->getOutput(0), lname + ".double_conv.4", 0);
	IActivationLayer* relu2 = network->addActivation(*bn2->getOutput(0), ActivationType::kLEAKY_RELU);
	assert(relu2);
	return relu2;
}

ILayer* down(INetworkDefinition *network, std::map<std::string, Weights>& weightMap, ITensor& input, int outch, int p, std::string lname) {

	IPoolingLayer* pool1 = network->addPoolingNd(input, PoolingType::kMAX, DimsHW{ 2, 2 });
	assert(pool1);
	ILayer* dcov1 = doubleConv(network, weightMap, *pool1->getOutput(0), outch, 3, lname + ".maxpool_conv.1", outch);
	assert(dcov1);
	return dcov1;
}

ILayer* up(INetworkDefinition *network, std::map<std::string, Weights>& weightMap, ITensor& input1, ITensor& input2, int resize, int outch, int midch, std::string lname) {
	float *deval = reinterpret_cast<float*>(malloc(sizeof(float) * resize * 2 * 2));
	for (int i = 0; i < resize * 2 * 2; i++) {
		deval[i] = 1.0;
	}
	ITensor* upsampleTensor;
	if (false) {
		Weights emptywts{ DataType::kFLOAT, nullptr, 0 };
		Weights deconvwts1{ DataType::kFLOAT, deval, resize * 2 * 2 };
		IDeconvolutionLayer* deconv1 = network->addDeconvolutionNd(input1, resize, DimsHW{ 2, 2 }, deconvwts1, emptywts);
		deconv1->setStrideNd(DimsHW{ 2, 2 });
		deconv1->setNbGroups(resize);
		weightMap["deconvwts." + lname] = deconvwts1;
		upsampleTensor = deconv1->getOutput(0);
	}
	else {
		IResizeLayer* resize = network->addResize(input1);
		std::vector<float> scale{ 1.f, 2, 2 };
		resize->setScales(scale.data(), scale.size());
		resize->setAlignCorners(true);
		resize->setResizeMode(ResizeMode::kLINEAR);
		upsampleTensor = resize->getOutput(0);
	}

	int diffx = input2.getDimensions().d[1] - upsampleTensor->getDimensions().d[1];
	int diffy = input2.getDimensions().d[2] - upsampleTensor->getDimensions().d[2];

	ILayer* pad1 = network->addPaddingNd(*upsampleTensor, DimsHW{ diffx / 2, diffy / 2 }, DimsHW{ diffx - (diffx / 2), diffy - (diffy / 2) });
	ITensor* inputTensors[] = { &input2,pad1->getOutput(0) };
	auto cat = network->addConcatenation(inputTensors, 2);
	assert(cat);
	if (midch == 64) {
		ILayer* dcov1 = doubleConv(network, weightMap, *cat->getOutput(0), outch, 3, lname + ".conv", outch);
		assert(dcov1);
		return dcov1;
	}
	else {
		int midch1 = outch / 2;
		ILayer* dcov1 = doubleConv(network, weightMap, *cat->getOutput(0), midch1, 3, lname + ".conv", outch);
		assert(dcov1);
		return dcov1;
	}
}

ILayer* outConv(INetworkDefinition *network, std::map<std::string, Weights>& weightMap, ITensor& input, int outch, std::string lname) {
	IConvolutionLayer* conv1 = network->addConvolutionNd(input, 2, DimsHW{ 1, 1 }, weightMap[lname + ".conv.weight"], weightMap[lname + ".conv.bias"]);
	assert(conv1);
	conv1->setStrideNd(DimsHW{ 1, 1 });
	conv1->setPaddingNd(DimsHW{ 0, 0 });
	conv1->setNbGroups(1);
	conv1->setName("[last_layer]"); // layer �̸� ����
	return conv1;
}


void createEngine(unsigned int maxBatchSize, IBuilder* builder, IBuilderConfig* config, DataType dt, char* engineFileName) {
	INetworkDefinition* network = builder->createNetworkV2(0U);

	std::map<std::string, Weights> weightMap = loadWeights("../Unet_py/unet.wts");
	Weights emptywts{ DataType::kFLOAT, nullptr, 0 };

	// Create input tensor of shape {3, INPUT_H, INPUT_W} with name INPUT_BLOB_NAME
	ITensor* data = network->addInput(INPUT_BLOB_NAME, dt, Dims3{ 3, INPUT_H, INPUT_W });
	assert(data);

	Preprocess preprocess{ maxBatchSize, INPUT_C, INPUT_H, INPUT_W, 0 };// Custom(preprocess) plugin ����ϱ�
	IPluginCreator* preprocess_creator = getPluginRegistry()->getPluginCreator("preprocess", "1");// Custom(preprocess) plugin�� global registry�� ��� �� plugin Creator ��ü ����
	IPluginV2 *preprocess_plugin = preprocess_creator->createPlugin("preprocess_plugin", (PluginFieldCollection*)&preprocess);// Custom(preprocess) plugin ����
	IPluginV2Layer* preprocess_layer = network->addPluginV2(&data, 1, *preprocess_plugin);// network ��ü�� custom(preprocess) plugin�� ����Ͽ� custom(preprocess) ���̾� �߰�
	preprocess_layer->setName("[preprocess_layer]"); // layer �̸� ����

	// build network
	auto x1 = doubleConv(network, weightMap, *preprocess_layer->getOutput(0), 64, 3, "inc", 64);
	auto x2 = down(network, weightMap, *x1->getOutput(0), 128, 1, "down1");
	auto x3 = down(network, weightMap, *x2->getOutput(0), 256, 1, "down2");
	auto x4 = down(network, weightMap, *x3->getOutput(0), 512, 1, "down3");
	auto x5 = down(network, weightMap, *x4->getOutput(0), 512, 1, "down4");
	ILayer* x6 = up(network, weightMap, *x5->getOutput(0), *x4->getOutput(0), 512, 512, 512, "up1");
	ILayer* x7 = up(network, weightMap, *x6->getOutput(0), *x3->getOutput(0), 256, 256, 256, "up2");
	ILayer* x8 = up(network, weightMap, *x7->getOutput(0), *x2->getOutput(0), 128, 128, 128, "up3");
	ILayer* x9 = up(network, weightMap, *x8->getOutput(0), *x1->getOutput(0), 64, 64, 64, "up4");
	ILayer* x10 = outConv(network, weightMap, *x9->getOutput(0), OUTPUT_SIZE, "outc");
	std::cout << "set name out" << std::endl;
	x10->getOutput(0)->setName(OUTPUT_BLOB_NAME);
	network->markOutput(*x10->getOutput(0));

	// Build engine
	builder->setMaxBatchSize(maxBatchSize);
	config->setMaxWorkspaceSize(16 * (1 << 20));  // 16MB
	if (precision_mode == 16) {
		std::cout << "==== precision f16 ====" << std::endl << std::endl;
		config->setFlag(BuilderFlag::kFP16);
	}
	else {
		std::cout << "==== precision f32 ====" << std::endl << std::endl;
	}
	std::cout << "Building engine, please wait for a while..." << std::endl;
	ICudaEngine* engine = builder->buildEngineWithConfig(*network, *config);

	std::cout << "==== model build done ====" << std::endl << std::endl;

	std::cout << "==== model selialize start ====" << std::endl << std::endl;

	IHostMemory* model_stream = engine->serialize();
	std::ofstream p(engineFileName, std::ios::binary);
	if (!p) {
		std::cerr << "could not open plan output file" << std::endl << std::endl;
		//return -1;
	}
	p.write(reinterpret_cast<const char*>(model_stream->data()), model_stream->size());

	model_stream->destroy();
	engine->destroy();
	std::cout << "==== model selialize done ====" << std::endl << std::endl;

	network->destroy();

	// Release host memory
	for (auto& mem : weightMap)
	{
		free((void*)(mem.second.values));
	}
}

int main()
{
	// ���� ���� 
	unsigned int maxBatchSize = 1;	// ������ TensorRT �������Ͽ��� ����� ��ġ ������ �� 
	bool serialize = false;			// Serialize ����ȭ ��Ű��(true ���� ���� ����)
	char engineFileName[] = "unet";
	char engine_file_path[256];
	sprintf(engine_file_path, "../Engine/%s_%d.engine", engineFileName, precision_mode);
	// 1) engine file ����� 
	// ���� ����� true�� ������ �ٽ� �����
	// ���� ����� false��, engine ���� ������ �ȸ���� 
	//					   engine ���� ������ ����
	bool exist_engine = false;
	if ((access(engine_file_path, 0) != -1)) {
		exist_engine = true;
	}

	if (!((serialize == false)/*Serialize ����ȭ ��*/ == (exist_engine == true) /*resnet18.engine ������ �ִ��� ����*/)) {
		std::cout << "===== Create Engine file =====" << std::endl << std::endl; // ���ο� ���� ����
		IBuilder* builder = createInferBuilder(gLogger);
		IBuilderConfig* config = builder->createBuilderConfig();
		createEngine(maxBatchSize, builder, config, DataType::kFLOAT, engine_file_path); // *** Trt �� ����� ***
		builder->destroy();
		config->destroy();
		std::cout << "===== Create Engine file =====" << std::endl << std::endl; // ���ο� ���� ���� �Ϸ�
	}

	// 2) engine file �ε� �ϱ� 
	char *trtModelStream{ nullptr };// ����� ��Ʈ���� ������ ����
	size_t size{ 0 };
	std::cout << "===== Engine file load =====" << std::endl << std::endl;
	std::ifstream file(engine_file_path, std::ios::binary);
	if (file.good()) {
		file.seekg(0, file.end);
		size = file.tellg();
		file.seekg(0, file.beg);
		trtModelStream = new char[size];
		file.read(trtModelStream, size);
		file.close();
	}
	else {
		std::cout << "[ERROR] Engine file load error" << std::endl;
	}

	// 3) file���� �ε��� stream���� tensorrt model ���� ����
	std::cout << "===== Engine file deserialize =====" << std::endl << std::endl;
	IRuntime* runtime = createInferRuntime(gLogger);
	ICudaEngine* engine = runtime->deserializeCudaEngine(trtModelStream, size);
	IExecutionContext* context = engine->createExecutionContext();
	delete[] trtModelStream;

	void* buffers[2];
	const int inputIndex = engine->getBindingIndex(INPUT_BLOB_NAME);
	const int outputIndex = engine->getBindingIndex(OUTPUT_BLOB_NAME);

	// GPU���� �Է°� ������� ����� �޸� �����Ҵ�
	CHECK(cudaMalloc(&buffers[inputIndex], maxBatchSize * INPUT_C * INPUT_H * INPUT_W * sizeof(uint8_t)));
	CHECK(cudaMalloc(&buffers[outputIndex], maxBatchSize * OUTPUT_SIZE * sizeof(float)));
	
	// CPU���� �Է°� ������� ����� �޸� �����Ҵ�
	std::vector<uint8_t> input(maxBatchSize * INPUT_H * INPUT_W * INPUT_C);	// �Է��� ��� �����̳� ���� ����
	std::vector<float> outputs(OUTPUT_SIZE);
	fromfile(input, "../Unet_py/input_data");

	// 4) �Է����� ����� �̹��� �غ��ϱ�
	//std::string img_dir = "../TestDate/";
	//std::vector<std::string> file_names;
	//if (SearchFile(img_dir.c_str(), file_names) < 0) { // �̹��� ���� ã��
	//	std::cerr << "[ERROR] Data search error" << std::endl;
	//}
	//else {
	//	std::cout << "Total number of images : " << file_names.size() << std::endl << std::endl;
	//}
	//cv::Mat img(INPUT_H, INPUT_W, CV_8UC3);
	//cv::Mat ori_img;
	//for (int idx = 0; idx < maxBatchSize; idx++) { // mat -> vector<uint8_t> 
	//	cv::Mat ori_img = cv::imread(file_names[idx]);
	//	cv::resize(ori_img, img, img.size()); // input size�� ��������
	//	memcpy(input.data(), img.data, maxBatchSize * INPUT_H * INPUT_W * INPUT_C);
	//}

	std::cout << "===== input load done =====" << std::endl << std::endl;

	uint64_t dur_time = 0;
	uint64_t iter_count = 100;

	// CUDA ��Ʈ�� ����
	cudaStream_t stream;
	CHECK(cudaStreamCreate(&stream));

	CHECK(cudaMemcpyAsync(buffers[inputIndex], input.data(), maxBatchSize * INPUT_C * INPUT_H * INPUT_W * sizeof(uint8_t), cudaMemcpyHostToDevice, stream));
	context->enqueue(maxBatchSize, buffers, stream, nullptr);
	CHECK(cudaMemcpyAsync(outputs.data(), buffers[outputIndex], maxBatchSize * OUTPUT_SIZE * sizeof(float), cudaMemcpyDeviceToHost, stream));
	cudaStreamSynchronize(stream);

	// 5) Inference ����  
	for (int i = 0; i < iter_count; i++) {
		// DMA input batch data to device, infer on the batch asynchronously, and DMA output back to host
		auto start = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();

		CHECK(cudaMemcpyAsync(buffers[inputIndex], input.data(), maxBatchSize * INPUT_C * INPUT_H * INPUT_W * sizeof(uint8_t), cudaMemcpyHostToDevice, stream));
		context->enqueue(maxBatchSize, buffers, stream, nullptr);
		CHECK(cudaMemcpyAsync(outputs.data(), buffers[outputIndex], maxBatchSize * OUTPUT_SIZE * sizeof(float), cudaMemcpyDeviceToHost, stream));
		cudaStreamSynchronize(stream);

		auto dur = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count() - start;
		dur_time += dur;
		//std::cout << dur << " milliseconds" << std::endl;
	}

	// 6) ��� ���
	std::cout << "==================================================" << std::endl;
	std::cout << "===============" << engineFileName << "===============" << std::endl;
	std::cout << iter_count << " th Iteration, Total dur time :: " << dur_time << " milliseconds" << std::endl;
	tofile(outputs);
	// �̹��� ��� ����

	std::cout << "==================================================" << std::endl;

	// Release stream and buffers ...
	cudaStreamDestroy(stream);
	CHECK(cudaFree(buffers[inputIndex]));
	CHECK(cudaFree(buffers[outputIndex]));
	context->destroy();
	engine->destroy();
	runtime->destroy();

	return 0;
}