#include <math.h>
#include <iostream>

#include "model.h"
#include "block.h"
#include "calibrator.h"
#include "config.h"


void printLayerDims(nvinfer1::ILayer* layer,const std::string& layerName) {
  // Get the dimensions of the layer's output.
  nvinfer1::Dims dims = layer->getOutput(0)->getDimensions();

  // Print the layer's name and output dimensions.
  std::cout << "name: " << layerName<< "  Layer name: " << layer->getName() << " Output Dims: ";
  for (int i = 0; i < dims.nbDims; ++i) {
    std::cout << dims.d[i] << (i < dims.nbDims - 1 ? "x" : "");
  }
  std::cout << std::endl;
}


void printTensorsDims(nvinfer1::ITensor* tensors[], int numTensors, const std::string& tensorsName) {
  for (int t = 0; t < numTensors; ++t) {
    std::cout << tensorsName << "[" << t << "]: ";
    if (tensors[t] != nullptr) {
      nvinfer1::Dims dims = tensors[t]->getDimensions();
      for (int i = 0; i < dims.nbDims; ++i) {
        std::cout << dims.d[i] << (i < dims.nbDims - 1 ? "x" : "");
      }
      std::cout << std::endl;
    } else {
      std::cout << "nullptr" << std::endl;
    }
  }
}


static int get_width(int x, float gw, int max_channels, int divisor = 8) {
    auto channel = int(ceil((x * gw) / divisor)) * divisor;
    return channel >= max_channels ? max_channels : channel;
}

static int get_depth(int x, float gd) {
    if (x == 1) return 1;
    int r = round(x * gd);
    if (x * gd - int(x * gd) == 0.5 && (int(x * gd) % 2) == 0) --r;
    return std::max<int>(r, 1);
}

static nvinfer1::IElementWiseLayer* Proto(nvinfer1::INetworkDefinition* network, std::map<std::string, nvinfer1::Weights>& weightMap,
                                          nvinfer1::ITensor& input, std::string lname, float gw, int max_channels) {
    int mid_channel = get_width(256, gw, max_channels);
    auto cv1 = convBnSiLU(network, weightMap, input, mid_channel, 3, 1, 1, "model.22.proto.cv1");
    float* convTranpsose_bais = (float*)weightMap["model.22.proto.upsample.bias"].values;
    int convTranpsose_bais_len = weightMap["model.22.proto.upsample.bias"].count;
    nvinfer1::Weights bias{nvinfer1::DataType::kFLOAT, convTranpsose_bais, convTranpsose_bais_len};
    auto convTranpsose  = network->addDeconvolutionNd(*cv1->getOutput(0), mid_channel, nvinfer1::DimsHW{2,2}, weightMap["model.22.proto.upsample.weight"], bias);
    assert(convTranpsose);
    convTranpsose->setStrideNd(nvinfer1::DimsHW{2, 2});
    auto cv2 = convBnSiLU(network,weightMap,*convTranpsose->getOutput(0), mid_channel, 3, 1, 1, "model.22.proto.cv2");
    auto cv3 = convBnSiLU(network,weightMap,*cv2->getOutput(0), 32, 1, 1, 0,"model.22.proto.cv3");
    assert(cv3);
    return cv3;
}

static nvinfer1::IShuffleLayer* ProtoCoef(nvinfer1::INetworkDefinition* network, std::map<std::string, nvinfer1::Weights>& weightMap,
                                          nvinfer1::ITensor& input, std::string lname, int grid_shape, float gw) {

    int mid_channle = 0;
    if(gw == 0.25 || gw== 0.5) {
        mid_channle = 32;
    } else if(gw == 0.75) {
        mid_channle = 48;
    } else if(gw == 1.00) {
        mid_channle = 64;
    } else if(gw == 1.25) {
        mid_channle = 80;
    }
    auto cv0 = convBnSiLU(network, weightMap, input, mid_channle, 3, 1, 1, lname + ".0");
    auto cv1 = convBnSiLU(network, weightMap, *cv0->getOutput(0), mid_channle, 3, 1, 1, lname + ".1");
    float* cv2_bais_value = (float*)weightMap[lname + ".2" + ".bias"].values;
    int cv2_bais_len = weightMap[lname + ".2" + ".bias"].count;
    nvinfer1::Weights cv2_bais{nvinfer1::DataType::kFLOAT, cv2_bais_value, cv2_bais_len};
    auto cv2 = network->addConvolutionNd(*cv1->getOutput(0), 32, nvinfer1::DimsHW{1, 1}, weightMap[lname + ".2" + ".weight"], cv2_bais);
    cv2->setStrideNd(nvinfer1::DimsHW{1, 1});
    nvinfer1::IShuffleLayer* cv2_shuffle = network->addShuffle(*cv2->getOutput(0));
    cv2_shuffle->setReshapeDimensions(nvinfer1::Dims2{ 32, grid_shape});
    return cv2_shuffle;
}

nvinfer1::IHostMemory* buildEngineYolov8Det(nvinfer1::IBuilder* builder,
                                            nvinfer1::IBuilderConfig* config, nvinfer1::DataType dt,
                                            const std::string& wts_path, float& gd, float& gw, int& max_channels) {
    std::map<std::string, nvinfer1::Weights> weightMap = loadWeights(wts_path);
    nvinfer1::INetworkDefinition* network = builder->createNetworkV2(0U);

    /*******************************************************************************************************
    ******************************************  YOLOV8 INPUT  **********************************************
    *******************************************************************************************************/
    nvinfer1::ITensor* data = network->addInput(kInputTensorName, dt, nvinfer1::Dims3{3, kInputH, kInputW});
    assert(data);

    /*******************************************************************************************************
    *****************************************  YOLOV8 BACKBONE  ********************************************
    *******************************************************************************************************/
    nvinfer1::IElementWiseLayer* conv0 = convBnSiLU(network, weightMap, *data, get_width(64, gw, max_channels), 3, 2, 1, "model.0");
        printLayerDims(conv0,"conv0");
    nvinfer1::IElementWiseLayer* conv1 = convBnSiLU(network, weightMap, *conv0->getOutput(0), get_width(128, gw, max_channels), 3, 2, 1, "model.1");
        printLayerDims(conv1,"conv1");
    // 11233
    nvinfer1::IElementWiseLayer* conv2 = C2F(network, weightMap, *conv1->getOutput(0), get_width(128, gw, max_channels), get_width(128, gw, max_channels), get_depth(3, gd), true, 0.5, "model.2");
        printLayerDims(conv2,"conv2");
    nvinfer1::IElementWiseLayer* conv3 = convBnSiLU(network, weightMap, *conv2->getOutput(0), get_width(256, gw, max_channels), 3, 2, 1, "model.3");
        printLayerDims(conv3,"conv3");
    // 22466
    nvinfer1::IElementWiseLayer* conv4 = C2F(network, weightMap, *conv3->getOutput(0), get_width(256, gw, max_channels), get_width(256, gw, max_channels), get_depth(6, gd), true, 0.5, "model.4");
        printLayerDims(conv4,"conv4");
    nvinfer1::IElementWiseLayer* conv5 = convBnSiLU(network, weightMap, *conv4->getOutput(0), get_width(512, gw, max_channels), 3, 2, 1, "model.5");
        printLayerDims(conv5,"conv5");
    // 22466
    nvinfer1::IElementWiseLayer* conv6 = C2F(network, weightMap, *conv5->getOutput(0), get_width(512, gw, max_channels), get_width(512, gw, max_channels), get_depth(6, gd), true, 0.5, "model.6");
    printLayerDims(conv6,"conv6");
    nvinfer1::IElementWiseLayer* conv7 = convBnSiLU(network, weightMap, *conv6->getOutput(0), get_width(1024, gw, max_channels), 3, 2, 1, "model.7");
    // 11233
    printLayerDims(conv7,"conv7");
    nvinfer1::IElementWiseLayer* conv8 = C2F(network, weightMap, *conv7->getOutput(0), get_width(1024, gw, max_channels), get_width(1024, gw, max_channels), get_depth(3, gd), true, 0.5, "model.8");
    printLayerDims(conv8,"conv8");
    nvinfer1::IElementWiseLayer* conv9 = SPPF(network, weightMap, *conv8->getOutput(0), get_width(1024, gw, max_channels), get_width(1024, gw, max_channels), 5, "model.9");
    printLayerDims(conv9,"conv9");
    /*******************************************************************************************************
    *********************************************  YOLOV8 HEAD  ********************************************
    *******************************************************************************************************/
    float scale[] = {1.0, 2.0, 2.0};
    nvinfer1::IResizeLayer* upsample10 = network->addResize(*conv9->getOutput(0));
    assert(upsample10);
    upsample10->setResizeMode(nvinfer1::ResizeMode::kNEAREST);
    upsample10->setScales(scale, 3);

    nvinfer1::ITensor* inputTensor11[] = {upsample10->getOutput(0), conv6->getOutput(0)};
    printTensorsDims(inputTensor11,2,"inputTensor11");
    nvinfer1::IConcatenationLayer* cat11 = network->addConcatenation(inputTensor11, 2);

    nvinfer1::IElementWiseLayer* conv12 = C2F(network, weightMap, *cat11->getOutput(0), get_width(512, gw, max_channels), get_width(512, gw, max_channels), get_depth(3, gd), false, 0.5, "model.12");

    nvinfer1::IResizeLayer* upsample13 = network->addResize(*conv12->getOutput(0));
    assert(upsample13);
    upsample13->setResizeMode(nvinfer1::ResizeMode::kNEAREST);
    upsample13->setScales(scale, 3);

    nvinfer1::ITensor* inputTensor14[] = {upsample13->getOutput(0), conv4->getOutput(0)};
    nvinfer1::IConcatenationLayer* cat14 = network->addConcatenation(inputTensor14, 2);


    nvinfer1::IElementWiseLayer* conv15 = C2F(network, weightMap, *cat14->getOutput(0), get_width(256, gw, max_channels), get_width(256, gw, max_channels), get_depth(3, gd), false, 0.5, "model.15");
    nvinfer1::IElementWiseLayer* conv16 = convBnSiLU(network, weightMap, *conv15->getOutput(0), get_width(256, gw, max_channels), 3, 2, 1, "model.16");
    nvinfer1::ITensor* inputTensor17[] = {conv16->getOutput(0), conv12->getOutput(0)};
    nvinfer1::IConcatenationLayer* cat17 = network->addConcatenation(inputTensor17, 2);
    nvinfer1::IElementWiseLayer* conv18 = C2F(network, weightMap, *cat17->getOutput(0), get_width(512, gw, max_channels), get_width(512, gw, max_channels), get_depth(3, gd), false, 0.5, "model.18");
    nvinfer1::IElementWiseLayer* conv19 = convBnSiLU(network, weightMap, *conv18->getOutput(0), get_width(512, gw, max_channels), 3, 2, 1, "model.19");
    nvinfer1::ITensor* inputTensor20[] = {conv19->getOutput(0), conv9->getOutput(0)};
    nvinfer1::IConcatenationLayer* cat20 = network->addConcatenation(inputTensor20, 2);
    nvinfer1::IElementWiseLayer* conv21 = C2F(network, weightMap, *cat20->getOutput(0), get_width(1024, gw, max_channels), get_width(1024, gw, max_channels), get_depth(3, gd), false, 0.5, "model.21");

    /*******************************************************************************************************
    *********************************************  YOLOV8 OUTPUT  ******************************************
    *******************************************************************************************************/
    int base_in_channel = (gw == 1.25) ? 80 : 64;
    int base_out_channel = (gw == 0.25) ? std::max(64, std::min(kNumClass, 100)) : get_width(256, gw, max_channels);

    // output0
    nvinfer1::IElementWiseLayer* conv22_cv2_0_0 = convBnSiLU(network, weightMap, *conv15->getOutput(0), base_in_channel, 3, 1, 1, "model.22.cv2.0.0");
    nvinfer1::IElementWiseLayer* conv22_cv2_0_1 = convBnSiLU(network, weightMap, *conv22_cv2_0_0->getOutput(0), base_in_channel, 3, 1, 1, "model.22.cv2.0.1");
    nvinfer1::IConvolutionLayer* conv22_cv2_0_2 = network->addConvolutionNd(*conv22_cv2_0_1->getOutput(0), 64, nvinfer1::DimsHW{1, 1}, weightMap["model.22.cv2.0.2.weight"], weightMap["model.22.cv2.0.2.bias"]);
    conv22_cv2_0_2->setStrideNd(nvinfer1::DimsHW{1, 1});
    conv22_cv2_0_2->setPaddingNd(nvinfer1::DimsHW{0, 0});
    nvinfer1::IElementWiseLayer* conv22_cv3_0_0 = convBnSiLU(network, weightMap, *conv15->getOutput(0),base_out_channel, 3, 1, 1, "model.22.cv3.0.0");
    nvinfer1::IElementWiseLayer* conv22_cv3_0_1 = convBnSiLU(network, weightMap, *conv22_cv3_0_0->getOutput(0), base_out_channel, 3, 1, 1, "model.22.cv3.0.1");
    nvinfer1::IConvolutionLayer* conv22_cv3_0_2 = network->addConvolutionNd(*conv22_cv3_0_1->getOutput(0), kNumClass, nvinfer1::DimsHW{1, 1}, weightMap["model.22.cv3.0.2.weight"], weightMap["model.22.cv3.0.2.bias"]);
    conv22_cv3_0_2->setStride(nvinfer1::DimsHW{1, 1});
    conv22_cv3_0_2->setPadding(nvinfer1::DimsHW{0, 0});
    nvinfer1::ITensor* inputTensor22_0[] = {conv22_cv2_0_2->getOutput(0), conv22_cv3_0_2->getOutput(0)};
    nvinfer1::IConcatenationLayer* cat22_0 = network->addConcatenation(inputTensor22_0, 2);

    // output1
    nvinfer1::IElementWiseLayer* conv22_cv2_1_0 = convBnSiLU(network, weightMap, *conv18->getOutput(0), base_in_channel, 3, 1, 1, "model.22.cv2.1.0");
    nvinfer1::IElementWiseLayer* conv22_cv2_1_1 = convBnSiLU(network, weightMap, *conv22_cv2_1_0->getOutput(0), base_in_channel, 3, 1, 1, "model.22.cv2.1.1");
    nvinfer1::IConvolutionLayer* conv22_cv2_1_2 = network->addConvolutionNd(*conv22_cv2_1_1->getOutput(0), 64, nvinfer1::DimsHW{1, 1}, weightMap["model.22.cv2.1.2.weight"], weightMap["model.22.cv2.1.2.bias"]);
    conv22_cv2_1_2->setStrideNd(nvinfer1::DimsHW{1, 1});
    conv22_cv2_1_2->setPaddingNd(nvinfer1::DimsHW{0, 0});
    nvinfer1::IElementWiseLayer* conv22_cv3_1_0 = convBnSiLU(network, weightMap, *conv18->getOutput(0), base_out_channel, 3, 1, 1, "model.22.cv3.1.0");
    nvinfer1::IElementWiseLayer* conv22_cv3_1_1 = convBnSiLU(network, weightMap, *conv22_cv3_1_0->getOutput(0), base_out_channel, 3, 1, 1, "model.22.cv3.1.1");
    nvinfer1::IConvolutionLayer* conv22_cv3_1_2 = network->addConvolutionNd(*conv22_cv3_1_1->getOutput(0), kNumClass, nvinfer1::DimsHW{1, 1}, weightMap["model.22.cv3.1.2.weight"], weightMap["model.22.cv3.1.2.bias"]);
    conv22_cv3_1_2->setStrideNd(nvinfer1::DimsHW{1, 1});
    conv22_cv3_1_2->setPaddingNd(nvinfer1::DimsHW{0, 0});
    nvinfer1::ITensor* inputTensor22_1[] = {conv22_cv2_1_2->getOutput(0), conv22_cv3_1_2->getOutput(0)};
    nvinfer1::IConcatenationLayer* cat22_1 = network->addConcatenation(inputTensor22_1, 2);

    // output2
    nvinfer1::IElementWiseLayer* conv22_cv2_2_0 = convBnSiLU(network, weightMap, *conv21->getOutput(0), base_in_channel, 3, 1, 1, "model.22.cv2.2.0");
    nvinfer1::IElementWiseLayer* conv22_cv2_2_1 = convBnSiLU(network, weightMap, *conv22_cv2_2_0->getOutput(0), base_in_channel, 3, 1, 1, "model.22.cv2.2.1");
    nvinfer1::IConvolutionLayer* conv22_cv2_2_2 = network->addConvolution(*conv22_cv2_2_1->getOutput(0), 64, nvinfer1::DimsHW{1, 1}, weightMap["model.22.cv2.2.2.weight"], weightMap["model.22.cv2.2.2.bias"]);
    nvinfer1::IElementWiseLayer* conv22_cv3_2_0 = convBnSiLU(network, weightMap, *conv21->getOutput(0), base_out_channel, 3, 1, 1, "model.22.cv3.2.0");
    nvinfer1::IElementWiseLayer* conv22_cv3_2_1 = convBnSiLU(network, weightMap, *conv22_cv3_2_0->getOutput(0), base_out_channel, 3, 1, 1, "model.22.cv3.2.1");
    nvinfer1::IConvolutionLayer* conv22_cv3_2_2 = network->addConvolution(*conv22_cv3_2_1->getOutput(0), kNumClass, nvinfer1::DimsHW{1, 1}, weightMap["model.22.cv3.2.2.weight"], weightMap["model.22.cv3.2.2.bias"]);
    nvinfer1::ITensor* inputTensor22_2[] = {conv22_cv2_2_2->getOutput(0), conv22_cv3_2_2->getOutput(0)};
    nvinfer1::IConcatenationLayer* cat22_2 = network->addConcatenation(inputTensor22_2, 2);

    /*******************************************************************************************************
    *********************************************  YOLOV8 DETECT  ******************************************
    *******************************************************************************************************/

    nvinfer1::IShuffleLayer* shuffle22_0 = network->addShuffle(*cat22_0->getOutput(0));
    shuffle22_0->setReshapeDimensions(nvinfer1::Dims2{64 + kNumClass, (kInputH / 8) * (kInputW / 8)});

    nvinfer1::ISliceLayer* split22_0_0 = network->addSlice(*shuffle22_0->getOutput(0), nvinfer1::Dims2{0, 0}, nvinfer1::Dims2{64, (kInputH / 8) * (kInputW / 8)}, nvinfer1::Dims2{1, 1});
    nvinfer1::ISliceLayer* split22_0_1 = network->addSlice(*shuffle22_0->getOutput(0), nvinfer1::Dims2{64, 0}, nvinfer1::Dims2{kNumClass, (kInputH / 8) * (kInputW / 8)}, nvinfer1::Dims2{1, 1});
    nvinfer1::IShuffleLayer* dfl22_0 = DFL(network, weightMap, *split22_0_0->getOutput(0), 4, (kInputH / 8) * (kInputW / 8), 1, 1, 0, "model.22.dfl.conv.weight");
    nvinfer1::ITensor* inputTensor22_dfl_0[] = {dfl22_0->getOutput(0), split22_0_1->getOutput(0)};
    nvinfer1::IConcatenationLayer* cat22_dfl_0 = network->addConcatenation(inputTensor22_dfl_0, 2);

    nvinfer1::IShuffleLayer* shuffle22_1 = network->addShuffle(*cat22_1->getOutput(0));
    shuffle22_1->setReshapeDimensions(nvinfer1::Dims2{64 + kNumClass, (kInputH / 16) * (kInputW / 16)});
    nvinfer1::ISliceLayer* split22_1_0 = network->addSlice(*shuffle22_1->getOutput(0), nvinfer1::Dims2{0, 0}, nvinfer1::Dims2{64, (kInputH / 16) * (kInputW / 16)}, nvinfer1::Dims2{1, 1});
    nvinfer1::ISliceLayer* split22_1_1 = network->addSlice(*shuffle22_1->getOutput(0), nvinfer1::Dims2{64, 0}, nvinfer1::Dims2{kNumClass, (kInputH / 16) * (kInputW / 16)}, nvinfer1::Dims2{1, 1});
    nvinfer1::IShuffleLayer* dfl22_1 = DFL(network, weightMap, *split22_1_0->getOutput(0), 4, (kInputH / 16) * (kInputW / 16), 1, 1, 0, "model.22.dfl.conv.weight");
    nvinfer1::ITensor* inputTensor22_dfl_1[] = {dfl22_1->getOutput(0), split22_1_1->getOutput(0)};
    nvinfer1::IConcatenationLayer* cat22_dfl_1 = network->addConcatenation(inputTensor22_dfl_1, 2);

    nvinfer1::IShuffleLayer* shuffle22_2 = network->addShuffle(*cat22_2->getOutput(0));
    shuffle22_2->setReshapeDimensions(nvinfer1::Dims2{64 + kNumClass, (kInputH / 32) * (kInputW / 32)});
    nvinfer1::ISliceLayer* split22_2_0 = network->addSlice(*shuffle22_2->getOutput(0), nvinfer1::Dims2{0, 0}, nvinfer1::Dims2{64, (kInputH / 32) * (kInputW / 32)}, nvinfer1::Dims2{1, 1});
    nvinfer1::ISliceLayer* split22_2_1 = network->addSlice(*shuffle22_2->getOutput(0), nvinfer1::Dims2{64, 0}, nvinfer1::Dims2{kNumClass, (kInputH / 32) * (kInputW / 32)}, nvinfer1::Dims2{1, 1});
    nvinfer1::IShuffleLayer* dfl22_2 = DFL(network, weightMap, *split22_2_0->getOutput(0), 4, (kInputH / 32) * (kInputW / 32), 1, 1, 0, "model.22.dfl.conv.weight");
    nvinfer1::ITensor* inputTensor22_dfl_2[] = {dfl22_2->getOutput(0), split22_2_1->getOutput(0)};
    nvinfer1::IConcatenationLayer* cat22_dfl_2 = network->addConcatenation(inputTensor22_dfl_2, 2);

    nvinfer1::IPluginV2Layer* yolo = addYoLoLayer(network, std::vector<nvinfer1::IConcatenationLayer *>{cat22_dfl_0, cat22_dfl_1, cat22_dfl_2});
    yolo->getOutput(0)->setName(kOutputTensorName);
    network->markOutput(*yolo->getOutput(0));

    builder->setMaxBatchSize(kBatchSize);
    config->setMaxWorkspaceSize(16 * (1 << 20));

#if defined(USE_FP16)
    config->setFlag(nvinfer1::BuilderFlag::kFP16);
#elif defined(USE_INT8)
    std::cout << "Your platform support int8: " << (builder->platformHasFastInt8() ? "true" : "false") << std::endl;
    assert(builder->platformHasFastInt8());
    config->setFlag(nvinfer1::BuilderFlag::kINT8);
    auto* calibrator = new Int8EntropyCalibrator2(1, kInputW, kInputH, "../coco_calib/", "int8calib.table", kInputTensorName);
    config->setInt8Calibrator(calibrator);
#endif

    std::cout << "Building engine, please wait for a while..." << std::endl;
    nvinfer1::IHostMemory* serialized_model = builder->buildSerializedNetwork(*network, *config);
    std::cout << "Build engine successfully!" << std::endl;

    delete network;

    for (auto &mem : weightMap){
        free((void *)(mem.second.values));
    }
    return serialized_model;
}

nvinfer1::IHostMemory* buildEngineYolov8DetP6(nvinfer1::IBuilder* builder,
                                              nvinfer1::IBuilderConfig* config, nvinfer1::DataType dt,
                                              const std::string& wts_path, float& gd, float& gw, int& max_channels) {
    std::map<std::string, nvinfer1::Weights> weightMap = loadWeights(wts_path);
    for (const auto& kv : weightMap) {
        if (kv.first.find("conv.weight") != std::string::npos || kv.first.find("linear.weight") != std::string::npos) { // 检查 conv.weight 或 linear.weight
            std::cout << "Weight name: " << kv.first << ", ";
            std::cout << "Count: " << kv.second.count << ", ";
            std::cout << "Type: " << (kv.second.type == nvinfer1::DataType::kFLOAT ? "FLOAT" :
                                      kv.second.type == nvinfer1::DataType::kHALF ? "HALF" : "INT8") << std::endl;
        }
    }

    nvinfer1::INetworkDefinition* network = builder->createNetworkV2(0U);
    std::cout << "gd: " << gd << ", gw: " << gw << std::endl;
    /*******************************************************************************************************
    ******************************************  YOLOV8 INPUT  **********************************************
    *******************************************************************************************************/
    nvinfer1::ITensor* data = network->addInput(kInputTensorName, dt, nvinfer1::Dims3{3, kInputH, kInputW});
    assert(data);
    /*******************************************************************************************************
    *****************************************  YOLOV8 BACKBONE  ********************************************
    *******************************************************************************************************/
    nvinfer1::IElementWiseLayer* conv0 = convBnSiLU(network, weightMap, *data, get_width(64, gw, max_channels), 3, 2, 1, "model.0");
    printLayerDims(conv0,"conv0");
    nvinfer1::IElementWiseLayer* conv1 = convBnSiLU(network, weightMap, *conv0->getOutput(0), get_width(128, gw, max_channels), 3, 2, 1, "model.1");
    printLayerDims(conv1,"conv1");
    // 11233
    nvinfer1::IElementWiseLayer* conv2 = C2F(network, weightMap, *conv1->getOutput(0), get_width(128, gw, max_channels), get_width(128, gw, max_channels), get_depth(3, gd), true, 0.5, "model.2");
    printLayerDims(conv2,"conv2");
    nvinfer1::IElementWiseLayer* conv3 = convBnSiLU(network, weightMap, *conv2->getOutput(0), get_width(256, gw, max_channels), 3, 2, 1, "model.3");
    // 22466
    printLayerDims(conv3,"conv3");
    nvinfer1::IElementWiseLayer* conv4 = C2F(network, weightMap, *conv3->getOutput(0), get_width(256, gw, max_channels), get_width(256, gw, max_channels), get_depth(6, gd), true, 0.5, "model.4");
    printLayerDims(conv4,"conv4");
    nvinfer1::IElementWiseLayer* conv5 = convBnSiLU(network, weightMap, *conv4->getOutput(0), get_width(512, gw, max_channels), 3, 2, 1, "model.5");
    // 22466
    printLayerDims(conv5,"conv5");
    nvinfer1::IElementWiseLayer* conv6 = C2F(network, weightMap, *conv5->getOutput(0), get_width(512, gw, max_channels), get_width(512, gw, max_channels), get_depth(6, gd), true, 0.5, "model.6");

    printLayerDims(conv6,"conv6");
    nvinfer1::IElementWiseLayer* conv7 = convBnSiLU(network, weightMap, *conv6->getOutput(0), get_width(768, gw, max_channels), 3, 2, 1, "model.7");
    printLayerDims(conv7,"conv7");
    nvinfer1::IElementWiseLayer* conv8 = C2F(network, weightMap, *conv7->getOutput(0), get_width(768, gw, max_channels), get_width(768, gw, max_channels), get_depth(3, gd), true, 0.5, "model.8");

    printLayerDims(conv8,"conv8");
    nvinfer1::IElementWiseLayer* conv9 = convBnSiLU(network, weightMap, *conv8->getOutput(0), get_width(1024, gw, max_channels), 3, 2, 1, "model.9");
    printLayerDims(conv9,"conv9");
    nvinfer1::IElementWiseLayer* conv10 = C2F(network, weightMap, *conv9->getOutput(0), get_width(1024, gw, max_channels), get_width(1024, gw, max_channels), get_depth(3, gd), true, 0.5, "model.10");

    printLayerDims(conv10,"conv10");
    nvinfer1::IElementWiseLayer* conv11 = SPPF(network, weightMap, *conv10->getOutput(0), get_width(1024, gw, max_channels), get_width(1024, gw, max_channels), 5, "model.11");
    printLayerDims(conv11,"conv11");

    /*******************************************************************************************************
    *********************************************  YOLOV8 HEAD  ********************************************
    *******************************************************************************************************/
    // Head
    float scale[] = {1.0, 2.0, 2.0}; // scale used for upsampling

    // P5
    nvinfer1::IResizeLayer* upsample12 = network->addResize(*conv11->getOutput(0));
    printLayerDims(upsample12,"upsample12");
    upsample12->setResizeMode(nvinfer1::ResizeMode::kNEAREST);
    upsample12->setScales(scale, 3);
    printLayerDims(upsample12,"upsample12 resize");
    nvinfer1::ITensor* concat13_inputs[] = {upsample12->getOutput(0), conv8->getOutput(0)};
    printTensorsDims(concat13_inputs,2,"concat13 inputs");
    nvinfer1::IConcatenationLayer* concat13 = network->addConcatenation(concat13_inputs, 2);
    printLayerDims(concat13,"concat13");
    nvinfer1::IElementWiseLayer* conv14 = C2(network, weightMap, *concat13->getOutput(0), get_width(768, gw, max_channels), get_width(768, gw, max_channels), get_depth(3, gd), false, 0.5, "model.14");
    printLayerDims(conv14,"conv14");



    // P4
    nvinfer1::IResizeLayer* upsample15 = network->addResize(*conv14->getOutput(0));
    printLayerDims(upsample15,"upsample15");
    upsample15->setResizeMode(nvinfer1::ResizeMode::kNEAREST);
    upsample15->setScales(scale, 3);
    printLayerDims(upsample15,"upsample15 resize");
    nvinfer1::ITensor* concat16_inputs[] = {upsample15->getOutput(0), conv6->getOutput(0)};
    printTensorsDims(concat16_inputs, 2, "concat16_inputs");
    nvinfer1::IConcatenationLayer* concat16 = network->addConcatenation(concat16_inputs, 2);
    printLayerDims(concat16,"concat16");
    nvinfer1::IElementWiseLayer* conv17 = C2(network, weightMap, *concat16->getOutput(0), get_width(512, gw, max_channels), get_width(512, gw, max_channels), get_depth(3, gd), false, 0.5, "model.17");
    printLayerDims(conv17,"conv17");

    // P3
    nvinfer1::IResizeLayer* upsample18 = network->addResize(*conv17->getOutput(0));
    printLayerDims(upsample18,"upsample18");
    upsample18->setResizeMode(nvinfer1::ResizeMode::kNEAREST);
    upsample18->setScales(scale, 3);
    printLayerDims(upsample18,"upsample18 resize");
    nvinfer1::ITensor* concat19_inputs[] = {upsample18->getOutput(0), conv4->getOutput(0)};
    printTensorsDims(concat19_inputs, 2, "concat19_inputs");
    nvinfer1::IConcatenationLayer* concat19 = network->addConcatenation(concat19_inputs, 2);
    printLayerDims(concat19,"concat19");
    nvinfer1::IElementWiseLayer* conv20 = C2(network, weightMap, *concat19->getOutput(0), get_width(256, gw, max_channels), get_width(256, gw, max_channels), get_depth(3, gd), false, 0.5, "model.20");
    printLayerDims(conv20,"conv20");

    // Additional layers for P4, P5, P6
    // P4/16-medium
    nvinfer1::IElementWiseLayer* conv21 = convBnSiLU(network, weightMap, *conv20->getOutput(0), get_width(256, gw, max_channels), 3, 2, 1, "model.21");
    nvinfer1::ITensor* concat22_inputs[] = {conv21->getOutput(0), conv17->getOutput(0)};
    nvinfer1::IConcatenationLayer* concat22 = network->addConcatenation(concat22_inputs, 2);
    nvinfer1::IElementWiseLayer* conv23 = C2(network, weightMap, *concat22->getOutput(0), get_width(512, gw, max_channels), get_width(512, gw, max_channels), get_depth(3, gd), false, 0.5, "model.23");
    printLayerDims(conv23,"conv23");


    // P5/32-large
    nvinfer1::IElementWiseLayer* conv24 = convBnSiLU(network, weightMap, *conv23->getOutput(0), get_width(512, gw, max_channels), 3, 2, 1, "model.24");
    nvinfer1::ITensor* concat25_inputs[] = {conv24->getOutput(0), conv14->getOutput(0)};
    nvinfer1::IConcatenationLayer* concat25 = network->addConcatenation(concat25_inputs, 2);
    nvinfer1::IElementWiseLayer* conv26 = C2(network, weightMap, *concat25->getOutput(0), get_width(768, gw, max_channels), get_width(768, gw, max_channels), get_depth(3, gd), false, 0.5, "model.26");
    printLayerDims(conv26,"conv26");


    // P6/64-xlarge
    nvinfer1::IElementWiseLayer* conv27 = convBnSiLU(network, weightMap, *conv26->getOutput(0), get_width(768, gw, max_channels), 3, 2, 1, "model.27");
    nvinfer1::ITensor* concat28_inputs[] = {conv27->getOutput(0), conv11->getOutput(0)};
    nvinfer1::IConcatenationLayer* concat28 = network->addConcatenation(concat28_inputs, 2);
    nvinfer1::IElementWiseLayer* conv29 = C2(network, weightMap, *concat28->getOutput(0), get_width(1024, gw, max_channels), get_width(1024, gw, max_channels), get_depth(3, gd), false, 0.5, "model.29");
    printLayerDims(conv29,"conv29");


    /*******************************************************************************************************
    *********************************************  YOLOV8 OUTPUT  ******************************************
    *******************************************************************************************************/
    int base_in_channel = (gw == 1.25) ? 80 : 64;
    int base_out_channel = (gw == 0.25) ? std::max(64, std::min(kNumClass, 100)) : get_width(256, gw, max_channels);

    // output0
    nvinfer1::IElementWiseLayer* conv30_cv2_0_0 = convBnSiLU(network, weightMap, *conv20->getOutput(0), base_in_channel, 3, 1, 1, "model.30.cv2.0.0");
    printLayerDims(conv30_cv2_0_0,"conv30_cv2_0_0");
    nvinfer1::IElementWiseLayer* conv30_cv2_0_1 = convBnSiLU(network, weightMap, *conv30_cv2_0_0->getOutput(0), base_in_channel, 3, 1, 1, "model.30.cv2.0.1");
    printLayerDims(conv30_cv2_0_1,"conv30_cv2_0_1");
    nvinfer1::IConvolutionLayer* conv30_cv2_0_2 = network->addConvolutionNd(*conv30_cv2_0_1->getOutput(0), 64, nvinfer1::DimsHW{1, 1}, weightMap["model.30.cv2.0.2.weight"], weightMap["model.30.cv2.0.2.bias"]);
    printLayerDims(conv30_cv2_0_2,"conv30_cv2_0_2");
    conv30_cv2_0_2->setStrideNd(nvinfer1::DimsHW{1, 1});
    printLayerDims(conv30_cv2_0_2,"conv30_cv2_0_2 setStrideNd(nvinfer1::DimsHW{1, 1})");

    conv30_cv2_0_2->setPaddingNd(nvinfer1::DimsHW{0, 0});
    printLayerDims(conv30_cv2_0_2,"conv30_cv2_0_2");

    nvinfer1::IElementWiseLayer* conv30_cv3_0_0 = convBnSiLU(network, weightMap, *conv20->getOutput(0),base_out_channel, 3, 1, 1, "model.30.cv3.0.0");
    printLayerDims(conv30_cv3_0_0,"conv30_cv3_0_0");

    nvinfer1::IElementWiseLayer* conv30_cv3_0_1 = convBnSiLU(network, weightMap, *conv30_cv3_0_0->getOutput(0), base_out_channel, 3, 1, 1, "model.30.cv3.0.1");
    printLayerDims(conv30_cv3_0_1,"conv30_cv3_0_1");
    nvinfer1::IConvolutionLayer* conv30_cv3_0_2 = network->addConvolutionNd(*conv30_cv3_0_1->getOutput(0), kNumClass, nvinfer1::DimsHW{1, 1}, weightMap["model.30.cv3.0.2.weight"], weightMap["model.30.cv3.0.2.bias"]);
    printLayerDims(conv30_cv3_0_2,"conv30_cv3_0_2");
    conv30_cv3_0_2->setStride(nvinfer1::DimsHW{1, 1});
    printLayerDims(conv30_cv3_0_2,"conv30_cv3_0_2");
    conv30_cv3_0_2->setPadding(nvinfer1::DimsHW{0, 0});
    printLayerDims(conv30_cv3_0_2,"conv30_cv3_0_2");
    nvinfer1::ITensor* inputTensor30_0[] = {conv30_cv2_0_2->getOutput(0), conv30_cv3_0_2->getOutput(0)};
    printTensorsDims(inputTensor30_0,2,"inputTensor30_0");
    nvinfer1::IConcatenationLayer* cat30_0 = network->addConcatenation(inputTensor30_0, 2);
    printLayerDims(cat30_0,"cat30_0");

    // output1
    nvinfer1::IElementWiseLayer* conv30_cv2_1_0 = convBnSiLU(network, weightMap, *conv23->getOutput(0), base_in_channel, 3, 1, 1, "model.30.cv2.1.0");
    printLayerDims(conv30_cv2_1_0,"conv30_cv2_1_0");
    nvinfer1::IElementWiseLayer* conv30_cv2_1_1 = convBnSiLU(network, weightMap, *conv30_cv2_1_0->getOutput(0), base_in_channel, 3, 1, 1, "model.30.cv2.1.1");
    printLayerDims(conv30_cv2_1_1,"conv30_cv2_1_1");
    nvinfer1::IConvolutionLayer* conv30_cv2_1_2 = network->addConvolutionNd(*conv30_cv2_1_1->getOutput(0), 64, nvinfer1::DimsHW{1, 1}, weightMap["model.30.cv2.1.2.weight"], weightMap["model.30.cv2.1.2.bias"]);
    printLayerDims(conv30_cv2_1_2,"conv30_cv2_1_2");
    conv30_cv2_1_2->setStrideNd(nvinfer1::DimsHW{1, 1});
    printLayerDims(conv30_cv2_1_2,"conv30_cv2_1_2 setStrideNd(nvinfer1::DimsHW{1, 1})");
    conv30_cv2_1_2->setPaddingNd(nvinfer1::DimsHW{0, 0});
    printLayerDims(conv30_cv2_1_2,"conv30_cv2_1_2");
    nvinfer1::IElementWiseLayer* conv30_cv3_1_0 = convBnSiLU(network, weightMap, *conv23->getOutput(0), base_out_channel, 3, 1, 1, "model.30.cv3.1.0");
    printLayerDims(conv30_cv3_1_0,"conv30_cv3_1_0");
    nvinfer1::IElementWiseLayer* conv30_cv3_1_1 = convBnSiLU(network, weightMap, *conv30_cv3_1_0->getOutput(0), base_out_channel, 3, 1, 1, "model.30.cv3.1.1");
    printLayerDims(conv30_cv3_1_1,"conv30_cv3_1_1");
    nvinfer1::IConvolutionLayer* conv30_cv3_1_2 = network->addConvolutionNd(*conv30_cv3_1_1->getOutput(0), kNumClass, nvinfer1::DimsHW{1, 1}, weightMap["model.30.cv3.1.2.weight"], weightMap["model.30.cv3.1.2.bias"]);
    printLayerDims(conv30_cv3_1_2,"conv30_cv3_1_2");
    conv30_cv3_1_2->setStrideNd(nvinfer1::DimsHW{1, 1});
    printLayerDims(conv30_cv3_1_2,"conv30_cv3_1_2 setStrideNd(nvinfer1::DimsHW{1, 1})");
    conv30_cv3_1_2->setPaddingNd(nvinfer1::DimsHW{0, 0});
    printLayerDims(conv30_cv3_1_2,"conv30_cv3_1_2");
    nvinfer1::ITensor* inputTensor30_1[] = {conv30_cv2_1_2->getOutput(0), conv30_cv3_1_2->getOutput(0)};
    printTensorsDims(inputTensor30_1,2,"inputTensor30_1");
    nvinfer1::IConcatenationLayer* cat30_1 = network->addConcatenation(inputTensor30_1, 2);
    printLayerDims(cat30_1,"cat30_1");

    // output2
    nvinfer1::IElementWiseLayer* conv30_cv2_2_0 = convBnSiLU(network, weightMap, *conv26->getOutput(0), base_in_channel, 3, 1, 1, "model.30.cv2.2.0");
    printLayerDims(conv30_cv2_2_0,"conv30_cv2_2_0");
    nvinfer1::IElementWiseLayer* conv30_cv2_2_1 = convBnSiLU(network, weightMap, *conv30_cv2_2_0->getOutput(0), base_in_channel, 3, 1, 1, "model.30.cv2.2.1");
    printLayerDims(conv30_cv2_2_1,"conv30_cv2_2_1");
    nvinfer1::IConvolutionLayer* conv30_cv2_2_2 = network->addConvolution(*conv30_cv2_2_1->getOutput(0), 64, nvinfer1::DimsHW{1, 1}, weightMap["model.30.cv2.2.2.weight"], weightMap["model.30.cv2.2.2.bias"]);
    printLayerDims(conv30_cv2_2_2,"conv30_cv2_2_2");
    conv30_cv2_2_2->setStrideNd(nvinfer1::DimsHW{1, 1});
    printLayerDims(conv30_cv2_2_2,"conv30_cv2_2_2 setStrideNd(nvinfer1::DimsHW{1, 1})");
    conv30_cv2_2_2->setPaddingNd(nvinfer1::DimsHW{0, 0});
    printLayerDims(conv30_cv2_2_2,"conv30_cv2_2_2");
    nvinfer1::IElementWiseLayer* conv30_cv3_2_0 = convBnSiLU(network, weightMap, *conv26->getOutput(0), base_out_channel, 3, 1, 1, "model.30.cv3.2.0");
    printLayerDims(conv30_cv3_2_0,"conv30_cv3_2_0");
    nvinfer1::IElementWiseLayer* conv30_cv3_2_1 = convBnSiLU(network, weightMap, *conv30_cv3_2_0->getOutput(0), base_out_channel, 3, 1, 1, "model.30.cv3.2.1");
    printLayerDims(conv30_cv3_2_1,"conv30_cv3_2_1");
    nvinfer1::IConvolutionLayer* conv30_cv3_2_2 = network->addConvolution(*conv30_cv3_2_1->getOutput(0), kNumClass, nvinfer1::DimsHW{1, 1}, weightMap["model.30.cv3.2.2.weight"], weightMap["model.30.cv3.2.2.bias"]);
    printLayerDims(conv30_cv3_2_2,"conv30_cv3_2_2");
    conv30_cv3_2_2->setStrideNd(nvinfer1::DimsHW{1, 1});
    printLayerDims(conv30_cv3_2_2,"conv30_cv3_2_2 setStrideNd(nvinfer1::DimsHW{1, 1})");
    conv30_cv3_2_2->setPaddingNd(nvinfer1::DimsHW{0, 0});
    printLayerDims(conv30_cv3_2_2,"conv30_cv3_2_2");
    nvinfer1::ITensor* inputTensor30_2[] = {conv30_cv2_2_2->getOutput(0), conv30_cv3_2_2->getOutput(0)};
    printTensorsDims(inputTensor30_2,2,"inputTensor30_2");
    nvinfer1::IConcatenationLayer* cat30_2 = network->addConcatenation(inputTensor30_2, 2);
    printLayerDims(cat30_2,"cat30_2");

    // output3
    nvinfer1::IElementWiseLayer * conv30_cv2_3_0 = convBnSiLU(network, weightMap, *conv29->getOutput(0), base_in_channel, 3, 1, 1, "model.30.cv2.3.0");
    printLayerDims(conv30_cv2_3_0,"conv30_cv2_3_0");
    nvinfer1::IElementWiseLayer * conv30_cv2_3_1 = convBnSiLU(network, weightMap, *conv30_cv2_3_0->getOutput(0), base_in_channel, 3, 1, 1, "model.30.cv2.3.1");
    printLayerDims(conv30_cv2_3_1,"conv30_cv2_3_1");
    nvinfer1::IConvolutionLayer * conv30_cv2_3_2 = network->addConvolution(*conv30_cv2_3_1->getOutput(0), 64, nvinfer1::DimsHW{1, 1}, weightMap["model.30.cv2.3.2.weight"], weightMap["model.30.cv2.3.2.bias"]);
    printLayerDims(conv30_cv2_3_2,"conv30_cv2_3_2");
    conv30_cv2_3_2->setStrideNd(nvinfer1::DimsHW{1, 1});
    printLayerDims(conv30_cv2_3_2,"conv30_cv2_3_2 setStrideNd(nvinfer1::DimsHW{1, 1})");
    conv30_cv2_3_2->setPaddingNd(nvinfer1::DimsHW{0, 0});
    printLayerDims(conv30_cv2_3_2,"conv30_cv2_3_2");
    nvinfer1::IElementWiseLayer * conv30_cv3_3_0 = convBnSiLU(network, weightMap, *conv29->getOutput(0), base_out_channel, 3, 1, 1, "model.30.cv3.3.0");
    printLayerDims(conv30_cv3_3_0,"conv30_cv3_3_0");
    nvinfer1::IElementWiseLayer * conv30_cv3_3_1 = convBnSiLU(network, weightMap, *conv30_cv3_3_0->getOutput(0), base_out_channel, 3, 1, 1, "model.30.cv3.3.1");
    printLayerDims(conv30_cv3_3_1,"conv30_cv3_3_1");
    nvinfer1::IConvolutionLayer * conv30_cv3_3_2 = network->addConvolution(*conv30_cv3_3_1->getOutput(0), kNumClass, nvinfer1::DimsHW{1, 1}, weightMap["model.30.cv3.3.2.weight"], weightMap["model.30.cv3.3.2.bias"]);
    printLayerDims(conv30_cv3_3_2,"conv30_cv3_3_2");
    conv30_cv3_3_2->setStrideNd(nvinfer1::DimsHW{1, 1});
    printLayerDims(conv30_cv3_3_2,"conv30_cv3_3_2 setStrideNd(nvinfer1::DimsHW{1, 1})");
    conv30_cv3_3_2->setPaddingNd(nvinfer1::DimsHW{0, 0});
    printLayerDims(conv30_cv3_3_2,"conv30_cv3_3_2");
    nvinfer1::ITensor * inputTensor30_3[] = {conv30_cv2_3_2->getOutput(0), conv30_cv3_3_2->getOutput(0)};
    printTensorsDims(inputTensor30_3,2,"inputTensor30_3");
    nvinfer1::IConcatenationLayer * cat30_3 = network->addConcatenation(inputTensor30_3, 2);
    printLayerDims(cat30_3,"cat30_3");




    /*******************************************************************************************************
    *********************************************  YOLOV8 DETECT  ******************************************
    *******************************************************************************************************/
    // P3 processing steps (remains unchanged)
    nvinfer1::IShuffleLayer* shuffle30_0 = network->addShuffle(*cat30_0->getOutput(0));  // Reusing the previous cat30_0 as P3 concatenation layer
    printLayerDims(shuffle30_0,"shuffle30_0");
    shuffle30_0->setReshapeDimensions(nvinfer1::Dims2{64 + kNumClass, (kInputH / 8) * (kInputW / 8)});
    printLayerDims(shuffle30_0,"shuffle30_0 setReshapeDimensions");
    nvinfer1::ISliceLayer* split30_0_0 = network->addSlice(*shuffle30_0->getOutput(0), nvinfer1::Dims2{0, 0}, nvinfer1::Dims2{64, (kInputH / 8) * (kInputW / 8)}, nvinfer1::Dims2{1, 1});
    printLayerDims(split30_0_0,"split30_0_0");
    nvinfer1::ISliceLayer* split30_0_1 = network->addSlice(*shuffle30_0->getOutput(0), nvinfer1::Dims2{64, 0}, nvinfer1::Dims2{kNumClass, (kInputH / 8) * (kInputW / 8)}, nvinfer1::Dims2{1, 1});
    printLayerDims(split30_0_1,"split30_0_1");
    nvinfer1::IShuffleLayer* dfl30_0 = DFL(network, weightMap, *split30_0_0->getOutput(0), 4, (kInputH / 8) * (kInputW / 8), 1, 1, 0, "model.30.dfl.conv.weight");
    printLayerDims(dfl30_0,"dfl30_0");
    nvinfer1::ITensor* inputTensor30_dfl_0[] = {dfl30_0->getOutput(0), split30_0_1->getOutput(0)};
    printTensorsDims(inputTensor30_dfl_0,2,"inputTensor30_dfl_0");
    nvinfer1::IConcatenationLayer* cat30_dfl_0 = network->addConcatenation(inputTensor30_dfl_0, 2);
    printLayerDims(cat30_dfl_0,"cat30_dfl_0");

    // P4 processing steps (remains unchanged)
    nvinfer1::IShuffleLayer* shuffle30_1 = network->addShuffle(*cat30_1->getOutput(0));  // Reusing the previous cat30_1 as P4 concatenation layer
    printLayerDims(shuffle30_1,"shuffle30_1");
    shuffle30_1->setReshapeDimensions(nvinfer1::Dims2{64 + kNumClass, (kInputH / 16) * (kInputW / 16)});
    printLayerDims(shuffle30_1,"shuffle30_1 setReshapeDimensions");
    nvinfer1::ISliceLayer* split30_1_0 = network->addSlice(*shuffle30_1->getOutput(0), nvinfer1::Dims2{0, 0}, nvinfer1::Dims2{64, (kInputH / 16) * (kInputW / 16)}, nvinfer1::Dims2{1, 1});
    printLayerDims(split30_1_0,"split30_1_0");
    nvinfer1::ISliceLayer* split30_1_1 = network->addSlice(*shuffle30_1->getOutput(0), nvinfer1::Dims2{64, 0}, nvinfer1::Dims2{kNumClass, (kInputH / 16) * (kInputW / 16)}, nvinfer1::Dims2{1, 1});
    printLayerDims(split30_1_1,"split30_1_1");
    nvinfer1::IShuffleLayer* dfl30_1 = DFL(network, weightMap, *split30_1_0->getOutput(0), 4, (kInputH / 16) * (kInputW / 16), 1, 1, 0, "model.30.dfl.conv.weight");
    printLayerDims(dfl30_1,"dfl30_1");
    nvinfer1::ITensor* inputTensor30_dfl_1[] = {dfl30_1->getOutput(0), split30_1_1->getOutput(0)};
    printTensorsDims(inputTensor30_dfl_1,2,"inputTensor30_dfl_1");
    nvinfer1::IConcatenationLayer* cat30_dfl_1 = network->addConcatenation(inputTensor30_dfl_1, 2);
    printLayerDims(cat30_dfl_1,"cat30_dfl_1");

    // P5 processing steps (remains unchanged)
    nvinfer1::IShuffleLayer* shuffle30_2 = network->addShuffle(*cat30_2->getOutput(0));  // Reusing the previous cat30_2 as P5 concatenation layer
    printLayerDims(shuffle30_2,"shuffle30_2");
    shuffle30_2->setReshapeDimensions(nvinfer1::Dims2{64 + kNumClass, (kInputH / 32) * (kInputW / 32)});
    printLayerDims(shuffle30_2,"shuffle30_2 setReshapeDimensions");
    nvinfer1::ISliceLayer* split30_2_0 = network->addSlice(*shuffle30_2->getOutput(0), nvinfer1::Dims2{0, 0}, nvinfer1::Dims2{64, (kInputH / 32) * (kInputW / 32)}, nvinfer1::Dims2{1, 1});
    printLayerDims(split30_2_0,"split30_2_0");
    nvinfer1::ISliceLayer* split30_2_1 = network->addSlice(*shuffle30_2->getOutput(0), nvinfer1::Dims2{64, 0}, nvinfer1::Dims2{kNumClass, (kInputH / 32) * (kInputW / 32)}, nvinfer1::Dims2{1, 1});
    printLayerDims(split30_2_1,"split30_2_1");
    nvinfer1::IShuffleLayer* dfl30_2 = DFL(network, weightMap, *split30_2_0->getOutput(0), 4, (kInputH / 32) * (kInputW / 32), 1, 1, 0, "model.30.dfl.conv.weight");
    printLayerDims(dfl30_2,"dfl30_2");
    nvinfer1::ITensor* inputTensor30_dfl_2[] = {dfl30_2->getOutput(0), split30_2_1->getOutput(0)};
    printTensorsDims(inputTensor30_dfl_2,2,"inputTensor30_dfl_2");
    nvinfer1::IConcatenationLayer* cat30_dfl_2 = network->addConcatenation(inputTensor30_dfl_2, 2);
    printLayerDims(cat30_dfl_2,"cat30_dfl_2");

    // P6 processing steps
    nvinfer1::IShuffleLayer* shuffle30_3 = network->addShuffle(*cat30_3->getOutput(0));
    printLayerDims(shuffle30_3,"shuffle30_3");
    shuffle30_3->setReshapeDimensions(nvinfer1::Dims2{64 + kNumClass, (kInputH / 64) * (kInputW / 64)});
    printLayerDims(shuffle30_3,"shuffle30_3 setReshapeDimensions");
    nvinfer1::ISliceLayer* split30_3_0 = network->addSlice(*shuffle30_3->getOutput(0), nvinfer1::Dims2{0, 0}, nvinfer1::Dims2{64, (kInputH / 64) * (kInputW / 64)}, nvinfer1::Dims2{1, 1});
    printLayerDims(split30_3_0,"split30_3_0");
    nvinfer1::ISliceLayer* split30_3_1 = network->addSlice(*shuffle30_3->getOutput(0), nvinfer1::Dims2{64, 0}, nvinfer1::Dims2{kNumClass, (kInputH / 64) * (kInputW / 64)}, nvinfer1::Dims2{1, 1});
    printLayerDims(split30_3_1,"split30_3_1");
    nvinfer1::IShuffleLayer* dfl30_3 = DFL(network, weightMap, *split30_3_0->getOutput(0), 4, (kInputH / 64) * (kInputW / 64), 1, 1, 0, "model.30.dfl.conv.weight");
    printLayerDims(dfl30_3,"dfl30_3");
    nvinfer1::ITensor* inputTensor30_dfl_3[] = {dfl30_3->getOutput(0), split30_3_1->getOutput(0)};
    printTensorsDims(inputTensor30_dfl_3,2,"inputTensor30_dfl_3");
    nvinfer1::IConcatenationLayer* cat30_dfl_3 = network->addConcatenation(inputTensor30_dfl_3, 2);
    printLayerDims(cat30_dfl_3,"cat30_dfl_3");

    nvinfer1::IPluginV2Layer* yolo = addYoLoLayer(network, std::vector<nvinfer1::IConcatenationLayer *>{cat30_dfl_0, cat30_dfl_1, cat30_dfl_2, cat30_dfl_3});
    printLayerDims(yolo,"yolo");
    yolo->getOutput(0)->setName(kOutputTensorName);
    network->markOutput(*yolo->getOutput(0));

    builder->setMaxBatchSize(kBatchSize);
    config->setMaxWorkspaceSize(16 * (1 << 20));

#if defined(USE_FP16)
    config->setFlag(nvinfer1::BuilderFlag::kFP16);
#elif defined(USE_INT8)
    std::cout << "Your platform support int8: " << (builder->platformHasFastInt8() ? "true" : "false") << std::endl;
    assert(builder->platformHasFastInt8());
    config->setFlag(nvinfer1::BuilderFlag::kINT8);
    auto* calibrator = new Int8EntropyCalibrator2(1, kInputW, kInputH, "../coco_calib/", "int8calib.table", kInputTensorName);
    config->setInt8Calibrator(calibrator);
#endif

    std::cout << "Building engine, please wait for a while..." << std::endl;
    nvinfer1::IHostMemory* serialized_model = builder->buildSerializedNetwork(*network, *config);
    std::cout << "Build engine successfully!" << std::endl;

    delete network;

    for (auto &mem : weightMap){
        free((void *)(mem.second.values));
    }
    return serialized_model;
}




nvinfer1::IHostMemory* buildEngineYolov8DetP2(nvinfer1::IBuilder* builder,
                                              nvinfer1::IBuilderConfig* config, nvinfer1::DataType dt,
                                              const std::string& wts_path, float& gd, float& gw, int& max_channels) {

  std::cout<<"buildEngineYolov8DetP2  "<<std::endl;


  std::map<std::string, nvinfer1::Weights> weightMap = loadWeights(wts_path);
  nvinfer1::INetworkDefinition* network = builder->createNetworkV2(0U);

  /*******************************************************************************************************
    ******************************************  YOLOV8 INPUT  **********************************************
    *******************************************************************************************************/
  nvinfer1::ITensor* data = network->addInput(kInputTensorName, dt, nvinfer1::Dims3{3, kInputH, kInputW});
  assert(data);

  /*******************************************************************************************************
    *****************************************  YOLOV8 BACKBONE  ********************************************
    *******************************************************************************************************/
  nvinfer1::IElementWiseLayer* conv0 = convBnSiLU(network, weightMap, *data, get_width(64, gw, max_channels), 3, 2, 1, "model.0");
      printLayerDims(conv0,"conv0");
  nvinfer1::IElementWiseLayer* conv1 = convBnSiLU(network, weightMap, *conv0->getOutput(0), get_width(128, gw, max_channels), 3, 2, 1, "model.1");
  printLayerDims(conv1,"conv1");
  // 11233
  nvinfer1::IElementWiseLayer* conv2 = C2F(network, weightMap, *conv1->getOutput(0), get_width(128, gw, max_channels), get_width(128, gw, max_channels), get_depth(3, gd), true, 0.5, "model.2");
  printLayerDims(conv2,"conv2");
  nvinfer1::IElementWiseLayer* conv3 = convBnSiLU(network, weightMap, *conv2->getOutput(0), get_width(256, gw, max_channels), 3, 2, 1, "model.3");
  printLayerDims(conv3,"conv3");
  // 22466
  nvinfer1::IElementWiseLayer* conv4 = C2F(network, weightMap, *conv3->getOutput(0), get_width(256, gw, max_channels), get_width(256, gw, max_channels), get_depth(6, gd), true, 0.5, "model.4");
  printLayerDims(conv4,"conv4");
  nvinfer1::IElementWiseLayer* conv5 = convBnSiLU(network, weightMap, *conv4->getOutput(0), get_width(512, gw, max_channels), 3, 2, 1, "model.5");
  printLayerDims(conv5,"conv5");
  // 22466
  nvinfer1::IElementWiseLayer* conv6 = C2F(network, weightMap, *conv5->getOutput(0), get_width(512, gw, max_channels), get_width(512, gw, max_channels), get_depth(6, gd), true, 0.5, "model.6");
  printLayerDims(conv6,"conv6");
  nvinfer1::IElementWiseLayer* conv7 = convBnSiLU(network, weightMap, *conv6->getOutput(0), get_width(1024, gw, max_channels), 3, 2, 1, "model.7");
  // 11233
  printLayerDims(conv7,"conv7");
  nvinfer1::IElementWiseLayer* conv8 = C2F(network, weightMap, *conv7->getOutput(0), get_width(1024, gw, max_channels), get_width(1024, gw, max_channels), get_depth(3, gd), true, 0.5, "model.8");
  printLayerDims(conv8,"conv8");
  nvinfer1::IElementWiseLayer* conv9 = SPPF(network, weightMap, *conv8->getOutput(0), get_width(1024, gw, max_channels), get_width(1024, gw, max_channels), 5, "model.9");
  printLayerDims(conv9,"conv9");


  /*******************************************************************************************************
    *********************************************  YOLOV8 HEAD  ********************************************
    *******************************************************************************************************/
  // Head
  float scale[] = {1.0, 2.0, 2.0}; // scale used for upsampling

  // P4
  nvinfer1::IResizeLayer* upsample10 = network->addResize(*conv9->getOutput(0)); // Assuming conv9 is the last layer of the backbone as per P5 in your first section.
  printLayerDims(upsample10,"upsample10");
  upsample10->setResizeMode(nvinfer1::ResizeMode::kNEAREST);
  upsample10->setScales(scale, 3);
  printLayerDims(upsample10,"upsample10 resize");
  nvinfer1::ITensor* concat11_inputs[] = {upsample10->getOutput(0), conv6->getOutput(0)}; // Assuming conv6 corresponds to "backbone P4" as per your pseudocode
  printTensorsDims(concat11_inputs, 2, "concat11_inputs");
  nvinfer1::IConcatenationLayer* concat11 = network->addConcatenation(concat11_inputs, 2);
  printLayerDims(concat11,"concat11");
  nvinfer1::IElementWiseLayer* conv12 = C2F(network, weightMap, *concat11->getOutput(0), get_width(512, gw, max_channels), get_width(512, gw, max_channels), get_depth(3, gd), false, 0.5, "model.12");
  printLayerDims(conv12,"conv12");




  // P3
  nvinfer1::IResizeLayer* upsample13 = network->addResize(*conv12->getOutput(0));
  printLayerDims(upsample13,"upsample13");
  upsample13->setResizeMode(nvinfer1::ResizeMode::kNEAREST);
  upsample13->setScales(scale, 3);
  printLayerDims(upsample13,"upsample13 resize");
  nvinfer1::ITensor* concat14_inputs[] = {upsample13->getOutput(0), conv4->getOutput(0)}; // Assuming conv4 corresponds to "backbone P3"
  printTensorsDims(concat14_inputs, 2, "concat14_inputs");
  nvinfer1::IConcatenationLayer* concat14 = network->addConcatenation(concat14_inputs, 2);
  printLayerDims(concat14,"concat14");
  nvinfer1::IElementWiseLayer* conv15 = C2F(network, weightMap, *concat14->getOutput(0), get_width(256, gw, max_channels), get_width(256, gw, max_channels), get_depth(3, gd), false, 0.5, "model.15");
  printLayerDims(conv15,"conv15");


  // P2
  nvinfer1::IResizeLayer* upsample16 = network->addResize(*conv15->getOutput(0));
  printLayerDims(upsample16,"upsample16");
  upsample16->setResizeMode(nvinfer1::ResizeMode::kNEAREST);
  upsample16->setScales(scale, 3);
  printLayerDims(upsample16,"upsample16 resize");
  nvinfer1::ITensor* concat17_inputs[] = {upsample16->getOutput(0), conv2->getOutput(0)}; // Assuming conv2 corresponds to "backbone P2"
  printTensorsDims(concat17_inputs, 2, "concat17_inputs");
  nvinfer1::IConcatenationLayer* concat17 = network->addConcatenation(concat17_inputs, 2);
  printLayerDims(concat17,"concat17");
  nvinfer1::IElementWiseLayer* conv18 = C2F(network, weightMap, *concat17->getOutput(0), get_width(128, gw, max_channels), get_width(128, gw, max_channels), get_depth(3, gd), false, 0.5, "model.18");
  printLayerDims(conv18,"conv18");


  // Additional layers for P3, P4, P5
  // Downsample and concatenate for P3
  nvinfer1::IElementWiseLayer* conv19 = convBnSiLU(network, weightMap, *conv18->getOutput(0), get_width(128, gw, max_channels), 3, 2, 1, "model.19");
  printLayerDims(conv19,"conv19");
  nvinfer1::ITensor* concat20_inputs[] = {conv19->getOutput(0), conv15->getOutput(0)}; // concatenate with higher-resolution feature map from P3
  printTensorsDims(concat20_inputs, 2, "concat20_inputs");
  nvinfer1::IConcatenationLayer* concat20 = network->addConcatenation(concat20_inputs, 2);
  printLayerDims(concat20,"concat20");
  nvinfer1::IElementWiseLayer* conv21 = C2F(network, weightMap, *concat20->getOutput(0), get_width(256, gw, max_channels), get_width(256, gw, max_channels), get_depth(3, gd), false, 0.5, "model.21");
  printLayerDims(conv21,"conv21");


  // Downsample and concatenate for P4
  nvinfer1::IElementWiseLayer* conv22 = convBnSiLU(network, weightMap, *conv21->getOutput(0), get_width(256, gw, max_channels), 3, 2, 1, "model.22");
  printLayerDims(conv22,"conv22");
  nvinfer1::ITensor* concat23_inputs[] = {conv22->getOutput(0), conv12->getOutput(0)}; // concatenate with higher-resolution feature map from P4
  printTensorsDims(concat23_inputs, 2, "concat23_inputs");
  nvinfer1::IConcatenationLayer* concat23 = network->addConcatenation(concat23_inputs, 2);
  printLayerDims(concat23,"concat23");
  nvinfer1::IElementWiseLayer* conv24 = C2F(network, weightMap, *concat23->getOutput(0), get_width(512, gw, max_channels), get_width(512, gw, max_channels), get_depth(3, gd), false, 0.5, "model.24");
  printLayerDims(conv24,"conv24");


  // Downsample and concatenate for P5
  nvinfer1::IElementWiseLayer* conv25 = convBnSiLU(network, weightMap, *conv24->getOutput(0), get_width(512, gw, max_channels), 3, 2, 1, "model.25");
  printLayerDims(conv25,"conv25");
  nvinfer1::ITensor* concat26_inputs[] = {conv25->getOutput(0), conv9->getOutput(0)}; // concatenate with higher-resolution feature map from P5
  printTensorsDims(concat26_inputs, 2, "concat26_inputs");
  nvinfer1::IConcatenationLayer* concat26 = network->addConcatenation(concat26_inputs, 2);
  printLayerDims(concat26,"concat26");
  nvinfer1::IElementWiseLayer* conv27 = C2F(network, weightMap, *concat26->getOutput(0), get_width(1024, gw, max_channels), get_width(1024, gw, max_channels), get_depth(3, gd), false, 0.5, "model.27");
  printLayerDims(conv27,"conv27");




  /*******************************************************************************************************
    *********************************************  YOLOV8 OUTPUT  ******************************************
    *******************************************************************************************************/
  int base_in_channel = 64;
  int base_out_channel = (gw == 0.25) ? std::max(64, std::min(kNumClass, 100)) : get_width(128, gw, max_channels);



  std::cout<<"base_in_channel is : "<<base_in_channel<<std::endl;
  std::cout<<"base_out_channel is : "<<base_out_channel<<std::endl;




  // output0
  nvinfer1::IElementWiseLayer* conv28_cv2_0_0 = convBnSiLU(network, weightMap, *conv18->getOutput(0), base_in_channel, 3, 1, 1, "model.28.cv2.0.0");
  printLayerDims(conv28_cv2_0_0,"conv28_cv2_0_0");
  nvinfer1::IElementWiseLayer* conv28_cv2_0_1 = convBnSiLU(network, weightMap, *conv28_cv2_0_0->getOutput(0), base_in_channel, 3, 1, 1, "model.28.cv2.0.1");
  printLayerDims(conv28_cv2_0_1,"conv28_cv2_0_1");
  nvinfer1::IConvolutionLayer* conv28_cv2_0_2 = network->addConvolutionNd(*conv28_cv2_0_1->getOutput(0), base_in_channel, nvinfer1::DimsHW{1, 1}, weightMap["model.28.cv2.0.2.weight"], weightMap["model.28.cv2.0.2.bias"]);
  printLayerDims(conv28_cv2_0_2,"conv28_cv2_0_2");
  conv28_cv2_0_2->setStrideNd(nvinfer1::DimsHW{1, 1});
  printLayerDims(conv28_cv2_0_2,"conv28_cv2_0_2 setStrideNd(nvinfer1::DimsHW{1, 1})");
  conv28_cv2_0_2->setPaddingNd(nvinfer1::DimsHW{0, 0});
  printLayerDims(conv28_cv2_0_2,"conv28_cv2_0_2 setPaddingNd(nvinfer1::DimsHW{0, 0})");
  nvinfer1::IElementWiseLayer* conv28_cv3_0_0 = convBnSiLU(network, weightMap, *conv18->getOutput(0),base_out_channel, 3, 1, 1, "model.28.cv3.0.0");
  printLayerDims(conv28_cv3_0_0,"conv28_cv3_0_0");
  nvinfer1::IElementWiseLayer* conv28_cv3_0_1 = convBnSiLU(network, weightMap, *conv28_cv3_0_0->getOutput(0), base_out_channel, 3, 1, 1, "model.28.cv3.0.1");
  printLayerDims(conv28_cv3_0_1,"conv28_cv3_0_1");
  nvinfer1::IConvolutionLayer* conv28_cv3_0_2 = network->addConvolutionNd(*conv28_cv3_0_1->getOutput(0), kNumClass, nvinfer1::DimsHW{1, 1}, weightMap["model.28.cv3.0.2.weight"], weightMap["model.28.cv3.0.2.bias"]);
  printLayerDims(conv28_cv3_0_2,"conv28_cv3_0_2");
  conv28_cv3_0_2->setStride(nvinfer1::DimsHW{1, 1});
  printLayerDims(conv28_cv3_0_2,"conv28_cv3_0_2 setStrideNd(nvinfer1::DimsHW{1, 1})");
  conv28_cv3_0_2->setPadding(nvinfer1::DimsHW{0, 0});
  printLayerDims(conv28_cv3_0_2,"conv28_cv3_0_2 setPaddingNd(nvinfer1::DimsHW{0, 0})");
  nvinfer1::ITensor* inputTensor28_0[] = {conv28_cv2_0_2->getOutput(0), conv28_cv3_0_2->getOutput(0)};
  printTensorsDims(inputTensor28_0, 2, "inputTensor28_0");
  nvinfer1::IConcatenationLayer* cat28_0 = network->addConcatenation(inputTensor28_0, 2);
  printLayerDims(cat28_0,"cat28_0");


  // output1
  nvinfer1::IElementWiseLayer* conv28_cv2_1_0 = convBnSiLU(network, weightMap, *conv21->getOutput(0), base_in_channel, 3, 1, 1, "model.28.cv2.1.0");
  printLayerDims(conv28_cv2_1_0,"conv28_cv2_1_0");
  nvinfer1::IElementWiseLayer* conv28_cv2_1_1 = convBnSiLU(network, weightMap, *conv28_cv2_1_0->getOutput(0), base_in_channel, 3, 1, 1, "model.28.cv2.1.1");
  printLayerDims(conv28_cv2_1_1,"conv28_cv2_1_1");
  nvinfer1::IConvolutionLayer* conv28_cv2_1_2 = network->addConvolutionNd(*conv28_cv2_1_1->getOutput(0), 64, nvinfer1::DimsHW{1, 1}, weightMap["model.28.cv2.1.2.weight"], weightMap["model.28.cv2.1.2.bias"]);
  printLayerDims(conv28_cv2_1_2,"conv28_cv2_1_2");
  conv28_cv2_1_2->setStrideNd(nvinfer1::DimsHW{1, 1});
  printLayerDims(conv28_cv2_1_2,"conv28_cv2_1_2 setStrideNd(nvinfer1::DimsHW{1, 1})");
  conv28_cv2_1_2->setPaddingNd(nvinfer1::DimsHW{0, 0});
  printLayerDims(conv28_cv2_1_2,"conv28_cv2_1_2 setPaddingNd(nvinfer1::DimsHW{0, 0})");
  nvinfer1::IElementWiseLayer* conv28_cv3_1_0 = convBnSiLU(network, weightMap, *conv21->getOutput(0), base_out_channel, 3, 1, 1, "model.28.cv3.1.0");
  printLayerDims(conv28_cv3_1_0,"conv28_cv3_1_0");
  nvinfer1::IElementWiseLayer* conv28_cv3_1_1 = convBnSiLU(network, weightMap, *conv28_cv3_1_0->getOutput(0), base_out_channel, 3, 1, 1, "model.28.cv3.1.1");
  printLayerDims(conv28_cv3_1_1,"conv28_cv3_1_1");
  nvinfer1::IConvolutionLayer* conv28_cv3_1_2 = network->addConvolutionNd(*conv28_cv3_1_1->getOutput(0), kNumClass, nvinfer1::DimsHW{1, 1}, weightMap["model.28.cv3.1.2.weight"], weightMap["model.28.cv3.1.2.bias"]);
  printLayerDims(conv28_cv3_1_2,"conv28_cv3_1_2");
  conv28_cv3_1_2->setStrideNd(nvinfer1::DimsHW{1, 1});
  printLayerDims(conv28_cv3_1_2,"conv28_cv3_1_2 setStrideNd(nvinfer1::DimsHW{1, 1})");
  conv28_cv3_1_2->setPaddingNd(nvinfer1::DimsHW{0, 0});
  printLayerDims(conv28_cv3_1_2,"conv28_cv3_1_2 setPaddingNd(nvinfer1::DimsHW{0, 0})");
  nvinfer1::ITensor* inputTensor28_1[] = {conv28_cv2_1_2->getOutput(0), conv28_cv3_1_2->getOutput(0)};
  printTensorsDims(inputTensor28_1, 2, "inputTensor28_1");
  nvinfer1::IConcatenationLayer* cat28_1 = network->addConcatenation(inputTensor28_1, 2);
  printLayerDims(cat28_1,"cat28_1");


  // output2
  nvinfer1::IElementWiseLayer* conv28_cv2_2_0 = convBnSiLU(network, weightMap, *conv24->getOutput(0), base_in_channel, 3, 1, 1, "model.28.cv2.2.0");
  printLayerDims(conv28_cv2_2_0,"conv28_cv2_2_0");
  nvinfer1::IElementWiseLayer* conv28_cv2_2_1 = convBnSiLU(network, weightMap, *conv28_cv2_2_0->getOutput(0), base_in_channel, 3, 1, 1, "model.28.cv2.2.1");
  printLayerDims(conv28_cv2_2_1,"conv28_cv2_2_1");
  nvinfer1::IConvolutionLayer* conv28_cv2_2_2 = network->addConvolution(*conv28_cv2_2_1->getOutput(0), 64, nvinfer1::DimsHW{1, 1}, weightMap["model.28.cv2.2.2.weight"], weightMap["model.28.cv2.2.2.bias"]);
  printLayerDims(conv28_cv2_2_2,"conv28_cv2_2_2");
  conv28_cv2_2_2->setStrideNd(nvinfer1::DimsHW{1, 1});
  printLayerDims(conv28_cv2_2_2,"conv28_cv2_2_2 setStrideNd(nvinfer1::DimsHW{1, 1})");
  conv28_cv2_2_2->setPaddingNd(nvinfer1::DimsHW{0, 0});
  printLayerDims(conv28_cv2_2_2,"conv28_cv2_2_2 setPaddingNd(nvinfer1::DimsHW{0, 0})");
  nvinfer1::IElementWiseLayer* conv28_cv3_2_0 = convBnSiLU(network, weightMap, *conv24->getOutput(0), base_out_channel, 3, 1, 1, "model.28.cv3.2.0");
  printLayerDims(conv28_cv3_2_0,"conv28_cv3_2_0");
  nvinfer1::IElementWiseLayer* conv28_cv3_2_1 = convBnSiLU(network, weightMap, *conv28_cv3_2_0->getOutput(0), base_out_channel, 3, 1, 1, "model.28.cv3.2.1");
  printLayerDims(conv28_cv3_2_1,"conv28_cv3_2_1");
  nvinfer1::IConvolutionLayer* conv28_cv3_2_2 = network->addConvolution(*conv28_cv3_2_1->getOutput(0), kNumClass, nvinfer1::DimsHW{1, 1}, weightMap["model.28.cv3.2.2.weight"], weightMap["model.28.cv3.2.2.bias"]);
  printLayerDims(conv28_cv3_2_2,"conv28_cv3_2_2");
  conv28_cv3_2_2->setStrideNd(nvinfer1::DimsHW{1, 1});
  printLayerDims(conv28_cv3_2_2,"conv28_cv3_2_2 setStrideNd(nvinfer1::DimsHW{1, 1})");
  conv28_cv3_2_2->setPaddingNd(nvinfer1::DimsHW{0, 0});
  printLayerDims(conv28_cv3_2_2,"conv28_cv3_2_2 setPaddingNd(nvinfer1::DimsHW{0, 0})");
  nvinfer1::ITensor* inputTensor28_2[] = {conv28_cv2_2_2->getOutput(0), conv28_cv3_2_2->getOutput(0)};
  printTensorsDims(inputTensor28_2, 2, "inputTensor28_2");
  nvinfer1::IConcatenationLayer* cat28_2 = network->addConcatenation(inputTensor28_2, 2);
  printLayerDims(cat28_2,"cat28_2");

  // output3
  nvinfer1::IElementWiseLayer * conv28_cv2_3_0 = convBnSiLU(network, weightMap, *conv27->getOutput(0), base_in_channel, 3, 1, 1, "model.28.cv2.3.0");
  printLayerDims(conv28_cv2_3_0,"conv28_cv2_3_0");
  nvinfer1::IElementWiseLayer * conv28_cv2_3_1 = convBnSiLU(network, weightMap, *conv28_cv2_3_0->getOutput(0), base_in_channel, 3, 1, 1, "model.28.cv2.3.1");
  printLayerDims(conv28_cv2_3_1,"conv28_cv2_3_1");
  nvinfer1::IConvolutionLayer * conv28_cv2_3_2 = network->addConvolution(*conv28_cv2_3_1->getOutput(0), 64, nvinfer1::DimsHW{1, 1}, weightMap["model.28.cv2.3.2.weight"], weightMap["model.28.cv2.3.2.bias"]);
  printLayerDims(conv28_cv2_3_2,"conv28_cv2_3_2");
  conv28_cv2_3_2->setStrideNd(nvinfer1::DimsHW{1, 1});
  printLayerDims(conv28_cv2_3_2,"conv28_cv2_3_2 setStrideNd(nvinfer1::DimsHW{1, 1})");
  conv28_cv2_3_2->setPaddingNd(nvinfer1::DimsHW{0, 0});
  printLayerDims(conv28_cv2_3_2,"conv28_cv2_3_2 setPaddingNd(nvinfer1::DimsHW{0, 0})");
  nvinfer1::IElementWiseLayer * conv28_cv3_3_0 = convBnSiLU(network, weightMap, *conv27->getOutput(0), base_out_channel, 3, 1, 1, "model.28.cv3.3.0");
  printLayerDims(conv28_cv3_3_0,"conv28_cv3_3_0");
  nvinfer1::IElementWiseLayer * conv28_cv3_3_1 = convBnSiLU(network, weightMap, *conv28_cv3_3_0->getOutput(0), base_out_channel, 3, 1, 1, "model.28.cv3.3.1");
  printLayerDims(conv28_cv3_3_1,"conv28_cv3_3_1");
  nvinfer1::IConvolutionLayer * conv28_cv3_3_2 = network->addConvolution(*conv28_cv3_3_1->getOutput(0), kNumClass, nvinfer1::DimsHW{1, 1}, weightMap["model.28.cv3.3.2.weight"], weightMap["model.28.cv3.3.2.bias"]);
  printLayerDims(conv28_cv3_3_2,"conv28_cv3_3_2");
  conv28_cv3_3_2->setStrideNd(nvinfer1::DimsHW{1, 1});
  printLayerDims(conv28_cv3_3_2,"conv28_cv3_3_2 setStrideNd(nvinfer1::DimsHW{1, 1})");
  conv28_cv3_3_2->setPaddingNd(nvinfer1::DimsHW{0, 0});
  printLayerDims(conv28_cv3_3_2,"conv28_cv3_3_2 setPaddingNd(nvinfer1::DimsHW{0, 0})");
  nvinfer1::ITensor * inputTensor28_3[] = {conv28_cv2_3_2->getOutput(0), conv28_cv3_3_2->getOutput(0)};
  printTensorsDims(inputTensor28_3, 2, "inputTensor28_3");
  nvinfer1::IConcatenationLayer * cat28_3 = network->addConcatenation(inputTensor28_3, 2);
  printLayerDims(cat28_3,"cat28_3");





  /*******************************************************************************************************
    *********************************************  YOLOV8 DETECT  ******************************************
    *******************************************************************************************************/
  // P2 processing steps (remains unchanged)
  std::cout<<"kNumClass is : "<<kNumClass<<std::endl;
  std::cout<<"kInputH is : "<<kInputH<<std::endl;
  nvinfer1::IShuffleLayer* shuffle28_0 = network->addShuffle(*cat28_0->getOutput(0));
  printLayerDims(shuffle28_0,"shuffle28_0");
  shuffle28_0->setReshapeDimensions(nvinfer1::Dims2{64 + kNumClass, (kInputH / 4) * (kInputW / 4)});
  printLayerDims(shuffle28_0,"shuffle28_0 setReshapeDimensions");
  nvinfer1::ISliceLayer* split28_0_0 = network->addSlice(*shuffle28_0->getOutput(0), nvinfer1::Dims2{0, 0}, nvinfer1::Dims2{64, (kInputH / 4) * (kInputW / 4)}, nvinfer1::Dims2{1, 1});
  printLayerDims(split28_0_0,"split28_0_0");
  nvinfer1::ISliceLayer* split28_0_1 = network->addSlice(*shuffle28_0->getOutput(0), nvinfer1::Dims2{64, 0}, nvinfer1::Dims2{kNumClass, (kInputH / 4) * (kInputW / 4)}, nvinfer1::Dims2{1, 1});
  printLayerDims(split28_0_1,"split28_0_1");
  nvinfer1::IShuffleLayer* dfl28_0 = DFL(network, weightMap, *split28_0_0->getOutput(0), 4, (kInputH / 4) * (kInputW / 4), 1, 1, 0, "model.28.dfl.conv.weight");
  printLayerDims(dfl28_0,"dfl28_0");
  nvinfer1::ITensor* inputTensor28_dfl_0[] = {dfl28_0->getOutput(0), split28_0_1->getOutput(0)};
  printTensorsDims(inputTensor28_dfl_0, 2, "inputTensor28_dfl_0");
  nvinfer1::IConcatenationLayer* cat28_dfl_0 = network->addConcatenation(inputTensor28_dfl_0, 2);
  printLayerDims(cat28_dfl_0,"cat28_dfl_0");



  // P3 processing steps (remains unchanged)
  nvinfer1::IShuffleLayer* shuffle28_1 = network->addShuffle(*cat28_1->getOutput(0));
  printLayerDims(shuffle28_1,"shuffle28_1");
  shuffle28_1->setReshapeDimensions(nvinfer1::Dims2{64 + kNumClass, (kInputH / 8) * (kInputW / 8)});
  printLayerDims(shuffle28_1,"shuffle28_1 setReshapeDimensions");
  nvinfer1::ISliceLayer* split28_1_0 = network->addSlice(*shuffle28_1->getOutput(0), nvinfer1::Dims2{0, 0}, nvinfer1::Dims2{64, (kInputH / 8) * (kInputW / 8)}, nvinfer1::Dims2{1, 1});
  printLayerDims(split28_1_0,"split28_1_0");
  nvinfer1::ISliceLayer* split28_1_1 = network->addSlice(*shuffle28_1->getOutput(0), nvinfer1::Dims2{64, 0}, nvinfer1::Dims2{kNumClass, (kInputH / 8) * (kInputW / 8)}, nvinfer1::Dims2{1, 1});
  printLayerDims(split28_1_1,"split28_1_1");
  nvinfer1::IShuffleLayer* dfl28_1 = DFL(network, weightMap, *split28_1_0->getOutput(0), 4, (kInputH / 8) * (kInputW / 8), 1, 1, 0, "model.28.dfl.conv.weight");
  printLayerDims(dfl28_1,"dfl28_1");
  nvinfer1::ITensor* inputTensor28_dfl_1[] = {dfl28_1->getOutput(0), split28_1_1->getOutput(0)};
  printTensorsDims(inputTensor28_dfl_1, 2, "inputTensor28_dfl_1");
  nvinfer1::IConcatenationLayer* cat28_dfl_1 = network->addConcatenation(inputTensor28_dfl_1, 2);
  printLayerDims(cat28_dfl_1,"cat28_dfl_1");


  // P4 processing steps (remains unchanged)
  nvinfer1::IShuffleLayer* shuffle28_2 = network->addShuffle(*cat28_2->getOutput(0));
  printLayerDims(shuffle28_2,"shuffle28_2");
  shuffle28_2->setReshapeDimensions(nvinfer1::Dims2{64 + kNumClass, (kInputH / 16) * (kInputW / 16)});
  printLayerDims(shuffle28_2,"shuffle28_2 setReshapeDimensions");
  nvinfer1::ISliceLayer* split28_2_0 = network->addSlice(*shuffle28_2->getOutput(0), nvinfer1::Dims2{0, 0}, nvinfer1::Dims2{64, (kInputH / 16) * (kInputW / 16)}, nvinfer1::Dims2{1, 1});
  printLayerDims(split28_2_0,"split28_2_0");
  nvinfer1::ISliceLayer* split28_2_1 = network->addSlice(*shuffle28_2->getOutput(0), nvinfer1::Dims2{64, 0}, nvinfer1::Dims2{kNumClass, (kInputH / 16) * (kInputW / 16)}, nvinfer1::Dims2{1, 1});
  printLayerDims(split28_2_1,"split28_2_1");
  nvinfer1::IShuffleLayer* dfl28_2 = DFL(network, weightMap, *split28_2_0->getOutput(0), 4, (kInputH / 16) * (kInputW / 16), 1, 1, 0, "model.28.dfl.conv.weight");
  printLayerDims(dfl28_2,"dfl28_2");
  nvinfer1::ITensor* inputTensor28_dfl_2[] = {dfl28_2->getOutput(0), split28_2_1->getOutput(0)};
  printTensorsDims(inputTensor28_dfl_2, 2, "inputTensor28_dfl_2");
  nvinfer1::IConcatenationLayer* cat28_dfl_2 = network->addConcatenation(inputTensor28_dfl_2, 2);
  printLayerDims(cat28_dfl_2,"cat28_dfl_2");


  // P5 processing steps
  nvinfer1::IShuffleLayer* shuffle28_3 = network->addShuffle(*cat28_3->getOutput(0));
  printLayerDims(shuffle28_3,"shuffle28_3");
  shuffle28_3->setReshapeDimensions(nvinfer1::Dims2{64 + kNumClass, (kInputH / 32) * (kInputW / 32)});
  printLayerDims(shuffle28_3,"shuffle28_3 setReshapeDimensions");
  nvinfer1::ISliceLayer* split28_3_0 = network->addSlice(*shuffle28_3->getOutput(0), nvinfer1::Dims2{0, 0}, nvinfer1::Dims2{64, (kInputH / 32) * (kInputW / 32)}, nvinfer1::Dims2{1, 1});
  printLayerDims(split28_3_0,"split28_3_0");
  nvinfer1::ISliceLayer* split28_3_1 = network->addSlice(*shuffle28_3->getOutput(0), nvinfer1::Dims2{64, 0}, nvinfer1::Dims2{kNumClass, (kInputH / 32) * (kInputW / 32)}, nvinfer1::Dims2{1, 1});
  printLayerDims(split28_3_1,"split28_3_1");
  nvinfer1::IShuffleLayer* dfl28_3 = DFL(network, weightMap, *split28_3_0->getOutput(0), 4, (kInputH / 32) * (kInputW / 32), 1, 1, 0, "model.28.dfl.conv.weight");
  printLayerDims(dfl28_3,"dfl28_3");
  nvinfer1::ITensor* inputTensor28_dfl_3[] = {dfl28_3->getOutput(0), split28_3_1->getOutput(0)};
  printTensorsDims(inputTensor28_dfl_3, 2, "inputTensor28_dfl_3");
  nvinfer1::IConcatenationLayer* cat28_dfl_3 = network->addConcatenation(inputTensor28_dfl_3, 2);
  printLayerDims(cat28_dfl_3,"cat28_dfl_3");



  nvinfer1::IPluginV2Layer* yolo = addYoLoLayer(network, std::vector<nvinfer1::IConcatenationLayer *>{cat28_dfl_0, cat28_dfl_1, cat28_dfl_2, cat28_dfl_3});
  printLayerDims(yolo,"yolo");
  yolo->getOutput(0)->setName(kOutputTensorName);
  network->markOutput(*yolo->getOutput(0));

  builder->setMaxBatchSize(kBatchSize);
  config->setMaxWorkspaceSize(16 * (1 << 20));

#if defined(USE_FP16)
  config->setFlag(nvinfer1::BuilderFlag::kFP16);
#elif defined(USE_INT8)
  std::cout << "Your platform support int8: " << (builder->platformHasFastInt8() ? "true" : "false") << std::endl;
  assert(builder->platformHasFastInt8());
  config->setFlag(nvinfer1::BuilderFlag::kINT8);
  auto* calibrator = new Int8EntropyCalibrator2(1, kInputW, kInputH, "../coco_calib/", "int8calib.table", kInputTensorName);
  config->setInt8Calibrator(calibrator);
#endif

  std::cout << "Building engine, please wait for a while..." << std::endl;
  nvinfer1::IHostMemory* serialized_model = builder->buildSerializedNetwork(*network, *config);
  std::cout << "Build engine successfully!" << std::endl;

  delete network;

  for (auto &mem : weightMap){
    free((void *)(mem.second.values));
  }
  return serialized_model;
}







nvinfer1::IHostMemory* buildEngineYolov8Cls(nvinfer1::IBuilder* builder,
                                            nvinfer1::IBuilderConfig* config, nvinfer1::DataType dt,
                                            const std::string& wts_path, float& gd, float& gw) {
    std::map<std::string, nvinfer1::Weights> weightMap = loadWeights(wts_path);
    nvinfer1::INetworkDefinition* network = builder->createNetworkV2(0U);
    int max_channels=1280;
    // ****************************************** YOLOV8 INPUT **********************************************
    nvinfer1::ITensor* data = network->addInput(kInputTensorName, dt, nvinfer1::Dims3{3, kClsInputH, kClsInputW});
    assert(data);

    // ***************************************** YOLOV8 BACKBONE ********************************************
    nvinfer1::IElementWiseLayer* conv0 = convBnSiLU(network, weightMap, *data, get_width(64, gw, max_channels), 3, 2, 1, "model.0");
    nvinfer1::IElementWiseLayer* conv1 = convBnSiLU(network, weightMap, *conv0->getOutput(0), get_width(128, gw, max_channels), 3, 2, 1, "model.1");
    // C2 Block (11233)
    nvinfer1::IElementWiseLayer* conv2 = C2F(network, weightMap, *conv1->getOutput(0), get_width(128, gw, max_channels), get_width(128, gw, max_channels), get_depth(3, gd), true, 0.5, "model.2");
    nvinfer1::IElementWiseLayer* conv3 = convBnSiLU(network, weightMap, *conv2->getOutput(0), get_width(256, gw, max_channels), 3, 2, 1, "model.3");
    // C2 Block Sequence (22466)
    nvinfer1::IElementWiseLayer* conv4 = C2F(network, weightMap, *conv3->getOutput(0), get_width(256, gw, max_channels), get_width(256, gw, max_channels), get_depth(6, gd), true, 0.5, "model.4");
    nvinfer1::IElementWiseLayer* conv5 = convBnSiLU(network, weightMap, *conv4->getOutput(0), get_width(512, gw, max_channels), 3, 2, 1, "model.5");
    // C2 Block Sequence (22466)
    nvinfer1::IElementWiseLayer* conv6 = C2F(network, weightMap, *conv5->getOutput(0), get_width(512, gw, max_channels), get_width(512, gw, max_channels), get_depth(6, gd), true, 0.5, "model.6");
    nvinfer1::IElementWiseLayer* conv7 = convBnSiLU(network, weightMap, *conv6->getOutput(0), get_width(1024, gw, max_channels), 3, 2, 1, "model.7");
    // C2 Block (11233)
    nvinfer1::IElementWiseLayer* conv8 = C2F(network, weightMap, *conv7->getOutput(0), get_width(1024, gw, max_channels), get_width(1024, gw, max_channels), get_depth(3, gd), true, 0.5, "model.8");


    // ********************************************* YOLOV8 HEAD *********************************************

    auto conv_class = convBnSiLU(network, weightMap, *conv8->getOutput(0), 1280, 1, 1, 1, "model.9.conv");
    // Adjusted code
    nvinfer1::Dims dims = conv_class->getOutput(0)->getDimensions(); // Obtain the dimensions of the output of conv_class
    assert(dims.nbDims == 3); // Make sure there are exactly 3 dimensions (channels, height, width)


    nvinfer1::IPoolingLayer* pool2 = network->addPoolingNd(*conv_class->getOutput(0), nvinfer1::PoolingType::kAVERAGE, nvinfer1::DimsHW{ dims.d[1], dims.d[2] });
    assert(pool2);

    // Fully connected layer declaration
    nvinfer1::IFullyConnectedLayer* yolo = network->addFullyConnected(*pool2->getOutput(0), kClsNumClass, weightMap["model.9.linear.weight"], weightMap["model.9.linear.bias"]);
    assert(yolo);

    // Set the name for the output tensor and mark it as network output
    yolo->getOutput(0)->setName(kOutputTensorName);
    network->markOutput(*yolo->getOutput(0));

    // Set the maximum batch size and workspace size
    builder->setMaxBatchSize(kBatchSize);
    config->setMaxWorkspaceSize(16 * (1 << 20));

    // Configuration according to the precision mode being used
#if defined(USE_FP16)
    config->setFlag(nvinfer1::BuilderFlag::kFP16);
#elif defined(USE_INT8)
    std::cout << "Your platform supports int8: " << (builder->platformHasFastInt8() ? "true" : "false") << std::endl;
    assert(builder->platformHasFastInt8());
    config->setFlag(nvinfer1::BuilderFlag::kINT8);
    auto* calibrator = new Int8EntropyCalibrator2(1, kClsInputW, kClsInputH, "../coco_calib/", "int8calib.table", kInputTensorName);
    config->setInt8Calibrator(calibrator);
#endif

    // Begin building the engine; this may take a while
    std::cout << "Building engine, please wait for a while..." << std::endl;
    nvinfer1::IHostMemory* serialized_model = builder->buildSerializedNetwork(*network, *config);
    std::cout << "Build engine successfully!" << std::endl;

    // Cleanup the network definition and allocated weights
    delete network;

    for (auto &mem : weightMap){
        free((void *)(mem.second.values));
    }
    return serialized_model;
}


nvinfer1::IHostMemory* buildEngineYolov8Seg(nvinfer1::IBuilder* builder,
                                            nvinfer1::IBuilderConfig* config, nvinfer1::DataType dt,
                                            const std::string& wts_path, float& gd, float& gw, int& max_channels) {
    std::map<std::string, nvinfer1::Weights> weightMap = loadWeights(wts_path);
    nvinfer1::INetworkDefinition* network = builder->createNetworkV2(0U);

    /*******************************************************************************************************
    ******************************************  YOLOV8 INPUT  **********************************************
    *******************************************************************************************************/
    nvinfer1::ITensor* data = network->addInput(kInputTensorName, dt, nvinfer1::Dims3{3, kInputH, kInputW});
    assert(data);

    /*******************************************************************************************************
    *****************************************  YOLOV8 BACKBONE  ********************************************
    *******************************************************************************************************/
    nvinfer1::IElementWiseLayer* conv0 = convBnSiLU(network, weightMap, *data, get_width(64, gw, max_channels), 3, 2, 1, "model.0");
    nvinfer1::IElementWiseLayer* conv1 = convBnSiLU(network, weightMap, *conv0->getOutput(0), get_width(128, gw, max_channels), 3, 2, 1, "model.1");
    nvinfer1::IElementWiseLayer* conv2 = C2F(network, weightMap, *conv1->getOutput(0), get_width(128, gw, max_channels), get_width(128, gw, max_channels), get_depth(3, gd), true, 0.5, "model.2");
    nvinfer1::IElementWiseLayer* conv3 = convBnSiLU(network, weightMap, *conv2->getOutput(0), get_width(256, gw, max_channels), 3, 2, 1, "model.3");
    nvinfer1::IElementWiseLayer* conv4 = C2F(network, weightMap, *conv3->getOutput(0), get_width(256, gw, max_channels), get_width(256, gw, max_channels), get_depth(6, gd), true, 0.5, "model.4");
    nvinfer1::IElementWiseLayer* conv5 = convBnSiLU(network, weightMap, *conv4->getOutput(0), get_width(512, gw, max_channels), 3, 2, 1, "model.5");
    nvinfer1::IElementWiseLayer* conv6 = C2F(network, weightMap, *conv5->getOutput(0), get_width(512, gw, max_channels), get_width(512, gw, max_channels), get_depth(6, gd), true, 0.5, "model.6");
    nvinfer1::IElementWiseLayer* conv7 = convBnSiLU(network, weightMap, *conv6->getOutput(0), get_width(1024, gw, max_channels), 3, 2, 1, "model.7");
    nvinfer1::IElementWiseLayer* conv8 = C2F(network, weightMap, *conv7->getOutput(0), get_width(1024, gw, max_channels), get_width(1024, gw, max_channels), get_depth(3, gd), true, 0.5, "model.8");
    nvinfer1::IElementWiseLayer* conv9 = SPPF(network, weightMap, *conv8->getOutput(0), get_width(1024, gw, max_channels), get_width(1024, gw, max_channels), 5, "model.9");

    /*******************************************************************************************************
    *********************************************  YOLOV8 HEAD  ********************************************
    *******************************************************************************************************/
    float scale[] = {1.0, 2.0, 2.0};
    nvinfer1::IResizeLayer* upsample10 = network->addResize(*conv9->getOutput(0));
    assert(upsample10);
    upsample10->setResizeMode(nvinfer1::ResizeMode::kNEAREST);
    upsample10->setScales(scale, 3);

    nvinfer1::ITensor* inputTensor11[] = {upsample10->getOutput(0), conv6->getOutput(0)};
    nvinfer1::IConcatenationLayer* cat11 = network->addConcatenation(inputTensor11, 2);
    nvinfer1::IElementWiseLayer* conv12 = C2F(network, weightMap, *cat11->getOutput(0), get_width(512, gw, max_channels), get_width(512, gw, max_channels), get_depth(3, gd), false, 0.5, "model.12");

    nvinfer1::IResizeLayer* upsample13 = network->addResize(*conv12->getOutput(0));
    assert(upsample13);
    upsample13->setResizeMode(nvinfer1::ResizeMode::kNEAREST);
    upsample13->setScales(scale, 3);

    nvinfer1::ITensor* inputTensor14[] = {upsample13->getOutput(0), conv4->getOutput(0)};
    nvinfer1::IConcatenationLayer* cat14 = network->addConcatenation(inputTensor14, 2);
    nvinfer1::IElementWiseLayer* conv15 = C2F(network, weightMap, *cat14->getOutput(0), get_width(256, gw, max_channels), get_width(256, gw, max_channels), get_depth(3, gd), false, 0.5, "model.15");
    nvinfer1::IElementWiseLayer* conv16 = convBnSiLU(network, weightMap, *conv15->getOutput(0), get_width(256, gw, max_channels), 3, 2, 1, "model.16");
    nvinfer1::ITensor* inputTensor17[] = {conv16->getOutput(0), conv12->getOutput(0)};
    nvinfer1::IConcatenationLayer* cat17 = network->addConcatenation(inputTensor17, 2);
    nvinfer1::IElementWiseLayer* conv18 = C2F(network, weightMap, *cat17->getOutput(0), get_width(512, gw, max_channels), get_width(512, gw, max_channels), get_depth(3, gd), false, 0.5, "model.18");
    nvinfer1::IElementWiseLayer* conv19 = convBnSiLU(network, weightMap, *conv18->getOutput(0), get_width(512, gw, max_channels), 3, 2, 1, "model.19");
    nvinfer1::ITensor* inputTensor20[] = {conv19->getOutput(0), conv9->getOutput(0)};
    nvinfer1::IConcatenationLayer* cat20 = network->addConcatenation(inputTensor20, 2);
    nvinfer1::IElementWiseLayer* conv21 = C2F(network, weightMap, *cat20->getOutput(0), get_width(1024, gw, max_channels), get_width(1024, gw, max_channels), get_depth(3, gd), false, 0.5, "model.21");

    /*******************************************************************************************************
    *********************************************  YOLOV8 OUTPUT  ******************************************
    *******************************************************************************************************/
    int base_in_channel = (gw == 1.25) ? 80 : 64;
    int base_out_channel = (gw == 0.25) ? std::max(64, std::min(kNumClass, 100)) : get_width(256, gw, max_channels);

    // output0
    nvinfer1::IElementWiseLayer* conv22_cv2_0_0 = convBnSiLU(network, weightMap, *conv15->getOutput(0), base_in_channel, 3, 1, 1, "model.22.cv2.0.0");
    nvinfer1::IElementWiseLayer* conv22_cv2_0_1 = convBnSiLU(network, weightMap, *conv22_cv2_0_0->getOutput(0), base_in_channel, 3, 1, 1, "model.22.cv2.0.1");
    nvinfer1::IConvolutionLayer* conv22_cv2_0_2 = network->addConvolutionNd(*conv22_cv2_0_1->getOutput(0), 64, nvinfer1::DimsHW{1, 1}, weightMap["model.22.cv2.0.2.weight"], weightMap["model.22.cv2.0.2.bias"]);
    conv22_cv2_0_2->setStrideNd(nvinfer1::DimsHW{1, 1});
    conv22_cv2_0_2->setPaddingNd(nvinfer1::DimsHW{0, 0});
    nvinfer1::IElementWiseLayer *conv22_cv3_0_0 = convBnSiLU(network, weightMap, *conv15->getOutput(0), base_out_channel, 3, 1, 1, "model.22.cv3.0.0");
    nvinfer1::IElementWiseLayer *conv22_cv3_0_1 = convBnSiLU(network, weightMap, *conv22_cv3_0_0->getOutput(0), base_out_channel, 3, 1, 1, "model.22.cv3.0.1");
    nvinfer1::IConvolutionLayer *conv22_cv3_0_2 = network->addConvolutionNd(*conv22_cv3_0_1->getOutput(0), kNumClass, nvinfer1::DimsHW{1, 1}, weightMap["model.22.cv3.0.2.weight"], weightMap["model.22.cv3.0.2.bias"]);
    conv22_cv3_0_2->setStride(nvinfer1::DimsHW{1, 1});
    conv22_cv3_0_2->setPadding(nvinfer1::DimsHW{0, 0});
    nvinfer1::ITensor* inputTensor22_0[] = {conv22_cv2_0_2->getOutput(0), conv22_cv3_0_2->getOutput(0)};
    nvinfer1::IConcatenationLayer* cat22_0 = network->addConcatenation(inputTensor22_0, 2);
    printLayerDims(conv22_cv2_0_2,"conv22_cv2_0_2");
    printLayerDims(conv22_cv3_0_2,"conv22_cv3_0_2");
    printLayerDims(cat22_0,"cat22_0");
    int NumberFeatureMapsCat22_0 = conv22_cv2_0_2->getNbOutputMaps() + conv22_cv3_0_2->getNbOutputMaps();
    std::cout << "conv22_cv2_0_2->getNbOutputMaps() : " << conv22_cv2_0_2->getNbOutputMaps() << std::endl;
    std::cout << "conv22_cv3_0_2->getNbOutputMaps() : " << conv22_cv3_0_2->getNbOutputMaps() << std::endl;
    std::cout << "cat22_0->getNbOutputs() : " << cat22_0->getNbOutputs() << std::endl;



    // output1
    nvinfer1::IElementWiseLayer* conv22_cv2_1_0 = convBnSiLU(network, weightMap, *conv18->getOutput(0), base_in_channel, 3, 1, 1, "model.22.cv2.1.0");
    nvinfer1::IElementWiseLayer* conv22_cv2_1_1 = convBnSiLU(network, weightMap, *conv22_cv2_1_0->getOutput(0), base_in_channel, 3, 1, 1, "model.22.cv2.1.1");
    nvinfer1::IConvolutionLayer* conv22_cv2_1_2 = network->addConvolutionNd(*conv22_cv2_1_1->getOutput(0), 64, nvinfer1::DimsHW{1, 1}, weightMap["model.22.cv2.1.2.weight"], weightMap["model.22.cv2.1.2.bias"]);
    conv22_cv2_1_2->setStrideNd(nvinfer1::DimsHW{1, 1});
    conv22_cv2_1_2->setPaddingNd(nvinfer1::DimsHW{0, 0});
    nvinfer1::IElementWiseLayer* conv22_cv3_1_0 = convBnSiLU(network, weightMap, *conv18->getOutput(0), base_out_channel, 3, 1, 1, "model.22.cv3.1.0");
    nvinfer1::IElementWiseLayer* conv22_cv3_1_1 = convBnSiLU(network, weightMap, *conv22_cv3_1_0->getOutput(0), base_out_channel, 3, 1, 1, "model.22.cv3.1.1");
    nvinfer1::IConvolutionLayer* conv22_cv3_1_2 = network->addConvolutionNd(*conv22_cv3_1_1->getOutput(0), kNumClass, nvinfer1::DimsHW{1, 1}, weightMap["model.22.cv3.1.2.weight"], weightMap["model.22.cv3.1.2.bias"]);
    conv22_cv3_1_2->setStrideNd(nvinfer1::DimsHW{1, 1});
    conv22_cv3_1_2->setPaddingNd(nvinfer1::DimsHW{0, 0});
    nvinfer1::ITensor* inputTensor22_1[] = {conv22_cv2_1_2->getOutput(0), conv22_cv3_1_2->getOutput(0)};
    nvinfer1::IConcatenationLayer* cat22_1 = network->addConcatenation(inputTensor22_1, 2);

    // output2
    nvinfer1::IElementWiseLayer* conv22_cv2_2_0 = convBnSiLU(network, weightMap, *conv21->getOutput(0), base_in_channel, 3, 1, 1, "model.22.cv2.2.0");
    nvinfer1::IElementWiseLayer* conv22_cv2_2_1 = convBnSiLU(network, weightMap, *conv22_cv2_2_0->getOutput(0), base_in_channel, 3, 1, 1, "model.22.cv2.2.1");
    nvinfer1::IConvolutionLayer* conv22_cv2_2_2 = network->addConvolution(*conv22_cv2_2_1->getOutput(0), 64, nvinfer1::DimsHW{1, 1}, weightMap["model.22.cv2.2.2.weight"], weightMap["model.22.cv2.2.2.bias"]);
    nvinfer1::IElementWiseLayer* conv22_cv3_2_0 = convBnSiLU(network, weightMap, *conv21->getOutput(0), base_out_channel, 3, 1, 1, "model.22.cv3.2.0");
    nvinfer1::IElementWiseLayer* conv22_cv3_2_1 = convBnSiLU(network, weightMap, *conv22_cv3_2_0->getOutput(0), base_out_channel, 3, 1, 1, "model.22.cv3.2.1");
    nvinfer1::IConvolutionLayer* conv22_cv3_2_2 = network->addConvolution(*conv22_cv3_2_1->getOutput(0), kNumClass, nvinfer1::DimsHW{1, 1}, weightMap["model.22.cv3.2.2.weight"], weightMap["model.22.cv3.2.2.bias"]);
    nvinfer1::ITensor* inputTensor22_2[] = {conv22_cv2_2_2->getOutput(0), conv22_cv3_2_2->getOutput(0)};
    nvinfer1::IConcatenationLayer* cat22_2 = network->addConcatenation(inputTensor22_2, 2);

    /*******************************************************************************************************
    *********************************************  YOLOV8 DETECT  ******************************************
    *******************************************************************************************************/

    nvinfer1::IShuffleLayer* shuffle22_0 = network->addShuffle(*cat22_0->getOutput(0));
    shuffle22_0->setReshapeDimensions(nvinfer1::Dims2{64 + kNumClass, (kInputH / 8) * (kInputW / 8)});
    printLayerDims(shuffle22_0,"shuffle22_0");
    nvinfer1::ISliceLayer* split22_0_0 = network->addSlice(*shuffle22_0->getOutput(0), nvinfer1::Dims2{0, 0}, nvinfer1::Dims2{64, (kInputH / 8) * (kInputW / 8)}, nvinfer1::Dims2{1, 1});
    nvinfer1::ISliceLayer* split22_0_1 = network->addSlice(*shuffle22_0->getOutput(0), nvinfer1::Dims2{64, 0}, nvinfer1::Dims2{kNumClass, (kInputH / 8) * (kInputW / 8)}, nvinfer1::Dims2{1, 1});
    printLayerDims(split22_0_1,"split22_0_1");
    nvinfer1::IShuffleLayer* dfl22_0 = DFL(network, weightMap, *split22_0_0->getOutput(0), 4, (kInputH / 8) * (kInputW / 8), 1, 1, 0, "model.22.dfl.conv.weight");

    nvinfer1::IShuffleLayer* shuffle22_1 = network->addShuffle(*cat22_1->getOutput(0));
    shuffle22_1->setReshapeDimensions(nvinfer1::Dims2{64 + kNumClass, (kInputH / 16) * (kInputW / 16)});
    nvinfer1::ISliceLayer* split22_1_0 = network->addSlice(*shuffle22_1->getOutput(0), nvinfer1::Dims2{0, 0}, nvinfer1::Dims2{64, (kInputH / 16) * (kInputW / 16)}, nvinfer1::Dims2{1, 1});
    nvinfer1::ISliceLayer* split22_1_1 = network->addSlice(*shuffle22_1->getOutput(0), nvinfer1::Dims2{64, 0}, nvinfer1::Dims2{kNumClass, (kInputH / 16) * (kInputW / 16)}, nvinfer1::Dims2{1, 1});
    nvinfer1::IShuffleLayer* dfl22_1 = DFL(network, weightMap, *split22_1_0->getOutput(0), 4, (kInputH / 16) * (kInputW / 16), 1, 1, 0, "model.22.dfl.conv.weight");

    nvinfer1::IShuffleLayer* shuffle22_2 = network->addShuffle(*cat22_2->getOutput(0));
    shuffle22_2->setReshapeDimensions(nvinfer1::Dims2{64 + kNumClass, (kInputH / 32) * (kInputW / 32)});
    nvinfer1::ISliceLayer* split22_2_0 = network->addSlice(*shuffle22_2->getOutput(0), nvinfer1::Dims2{0, 0}, nvinfer1::Dims2{64, (kInputH / 32) * (kInputW / 32)}, nvinfer1::Dims2{1, 1});
    nvinfer1::ISliceLayer* split22_2_1 = network->addSlice(*shuffle22_2->getOutput(0), nvinfer1::Dims2{64, 0}, nvinfer1::Dims2{kNumClass, (kInputH / 32) * (kInputW / 32)}, nvinfer1::Dims2{1, 1});
    nvinfer1::IShuffleLayer* dfl22_2 = DFL(network, weightMap, *split22_2_0->getOutput(0), 4, (kInputH / 32) * (kInputW / 32), 1, 1, 0, "model.22.dfl.conv.weight");

    // det0
    auto proto_coef_0 = ProtoCoef(network, weightMap, *conv15->getOutput(0), "model.22.cv4.0", 6400, gw);
    printLayerDims(proto_coef_0,"proto_coef_0");
    nvinfer1::ITensor* inputTensor22_dfl_0[] = { dfl22_0->getOutput(0), split22_0_1->getOutput(0),proto_coef_0->getOutput(0)};
    nvinfer1::IConcatenationLayer *cat22_dfl_0 = network->addConcatenation(inputTensor22_dfl_0, 3);

    // det1
    auto proto_coef_1 = ProtoCoef(network, weightMap, *conv18->getOutput(0), "model.22.cv4.1", 1600, gw);
    nvinfer1::ITensor* inputTensor22_dfl_1[] = { dfl22_1->getOutput(0), split22_1_1->getOutput(0),proto_coef_1->getOutput(0)};
    nvinfer1::IConcatenationLayer *cat22_dfl_1 = network->addConcatenation(inputTensor22_dfl_1, 3);

    // det2
    auto proto_coef_2 = ProtoCoef(network, weightMap, *conv21->getOutput(0), "model.22.cv4.2", 400, gw);
    nvinfer1::ITensor* inputTensor22_dfl_2[] = { dfl22_2->getOutput(0), split22_2_1->getOutput(0) ,proto_coef_2->getOutput(0)};
    nvinfer1::IConcatenationLayer *cat22_dfl_2 = network->addConcatenation(inputTensor22_dfl_2, 3);


    nvinfer1::IPluginV2Layer* yolo = addYoLoLayer(network, std::vector<nvinfer1::IConcatenationLayer *>{cat22_dfl_0, cat22_dfl_1, cat22_dfl_2}, true);
    yolo->getOutput(0)->setName(kOutputTensorName);
    network->markOutput(*yolo->getOutput(0));

    auto proto = Proto(network, weightMap, *conv15->getOutput(0), "model.22.proto", gw, max_channels);
    proto->getOutput(0)->setName("proto");
    network->markOutput(*proto->getOutput(0));

    builder->setMaxBatchSize(kBatchSize);
    config->setMaxWorkspaceSize(16 * (1 << 20));

#if defined(USE_FP16)
    config->setFlag(nvinfer1::BuilderFlag::kFP16);
#elif defined(USE_INT8)
    std::cout << "Your platform support int8: " << (builder->platformHasFastInt8() ? "true" : "false") << std::endl;
    assert(builder->platformHasFastInt8());
    config->setFlag(nvinfer1::BuilderFlag::kINT8);
    auto* calibrator = new Int8EntropyCalibrator2(1, kInputW, kInputH, "../coco_calib/", "int8calib.table", kInputTensorName);
    config->setInt8Calibrator(calibrator);
#endif

    std::cout << "Building engine, please wait for a while..." << std::endl;
    nvinfer1::IHostMemory* serialized_model = builder->buildSerializedNetwork(*network, *config);
    std::cout << "Build engine successfully!" << std::endl;

    delete network;

    for (auto& mem : weightMap) {
        free((void*)(mem.second.values));
    }
    return serialized_model;
}