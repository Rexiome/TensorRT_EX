# TensorRT_EX


## TensorRT 작업 순서 
0. 학습 프레임 워크에서 학습 완료된 모델을 준비(trt에서 사용 할 웨이트 파일 생성).     
1-1. TensorRT API를 사용하여 학습된 모델 구조와 동일하게 모델을 구현(코딩).     
1-2. 기존 학습 모델에서 웨이트를 추출하여 준비된 TensorRT 모델의 각각 레이어에 알맞게 웨이트를 전달.     
2. 완성된 코드를 빌드하고 실행.     
3. TensorRT 모델이 빌드되고 난 뒤 생성된 모델 스트림 Serialize 하여 엔진 파일로 생성.     
4. 이 후 작업에서 엔진파일만 로드하여 추론 수행(만약 모델 파리미터나 레이어 수정시 3번 작업 재실행).     


## tensorrtx vgg 구현체 이용 TensorRT 기본 예제 만들기 (진행중)
- 사용하기 편한 구조 (엔진 파일 유무에 따라 재생성 또는 엔진 파일 로드 작업 수행)
- 좀 더 쉽고 직관적 코드 구조
- TRT 모델용 웨이트 파일의 웨이트 구조를 딕셔너리에서 리스트로 변경 (파일 용량 50% 감소)
- 입력 이미지 전처리 custom layer 예제 만들기 (custom plugin)(c++ cpu 코드와 결과값 비교 검증 필요)

vgg 예제 -> (refactoring) -> 기본 예제 -> (custom plugin) ->plugin 예제 


## reference   
tensorrtx : https://github.com/wang-xinyu/tensorrtx

