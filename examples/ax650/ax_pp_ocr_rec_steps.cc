/*
 * AXERA is pleased to support the open source community by making ax-samples available.
 *
 * Copyright (c) 2022, AXERA Semiconductor (Shanghai) Co., Ltd. All rights reserved.
 *
 * Licensed under the BSD 3-Clause License (the "License"); you may not use this file except
 * in compliance with the License. You may obtain a copy of the License at
 *
 * https://opensource.org/licenses/BSD-3-Clause
 *
 * Unless required by applicable law or agreed to in writing, software distributed
 * under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
 * CONDITIONS OF ANY KIND, either express or implied. See the License for the
 * specific language governing permissions and limitations under the License.
 */

/*
 * Author: ZHEQIUSHUI
 */

#include <cstdio>
#include <cstring>
#include <numeric>

#include <opencv2/opencv.hpp>
#include "base/common.hpp"
#include "middleware/io.hpp"

#include "utilities/args.hpp"
#include "utilities/cmdline.hpp"
#include "utilities/file.hpp"
#include "utilities/timer.hpp"

#include <ax_sys_api.h>
#include <ax_engine_api.h>

#include "base/score.hpp"
#include "base/topk.hpp"

const int DEFAULT_IMG_H = 48;
const int DEFAULT_IMG_W = 320;
const int DEFAULT_LOOP_COUNT = 1;

namespace ax
{
    std::vector<std::string> ReadDict(const std::string &path)
    {
        std::ifstream in(path);
        std::string line;
        std::vector<std::string> m_vec;
        if (in)
        {
            while (getline(in, line))
            {
                m_vec.push_back(line);
            }
        }
        else
        {
            std::cout << "no such label file: " << path << ", exit the program..."
                      << std::endl;
            exit(1);
        }
        return m_vec;
    }

    template <class ForwardIterator>
    inline static size_t argmax(ForwardIterator first, ForwardIterator last, ForwardIterator val)
    {
        ForwardIterator max_val = std::max_element(first, last);
        *val = *max_val;
        return std::distance(first, max_val);
    }

    void post_process(AX_ENGINE_IO_INFO_T *io_info, AX_ENGINE_IO_T *io_data, const cv::Mat &mat, std::vector<std::string> &label_list_, const std::vector<float> &time_costs)
    {
        timer timer_postprocess;

        auto &output = io_data->pOutputs[0];
        auto &info = io_info->pOutputs[0];
        auto ptr = (float *)output.pVirAddr;

        int cnt = info.pShape[1];
        int width = info.pShape[2];

        std::string str_res;
        int argmax_idx;
        int last_index = 0;
        float score = 0.f;
        int count = 0;
        float max_value = 0.0f;

        for (int n = 0; n < info.pShape[1]; n++)
        {
            printf("%f\n",ptr[0]);
            // get idx
            argmax_idx = int(argmax(
                &ptr[(1 * info.pShape[1] + n) * info.pShape[2]],
                &ptr[(1 * info.pShape[1] + n + 1) * info.pShape[2]], &max_value));
            // get score
            // max_value = float(*std::max_element(
            //     &ptr[(1 * info.pShape[1] + n) * info.pShape[2]],
            //     &ptr[(1 * info.pShape[1] + n + 1) * info.pShape[2]]));

            if (argmax_idx > 0 && (!(n > 0 && argmax_idx == last_index)))
            {
                printf("%d %.4f\n", argmax_idx, max_value);
                score += max_value;
                count += 1;
                str_res += label_list_[argmax_idx];
            }
            last_index = argmax_idx;
        }

        fprintf(stdout, "ppocr rec:%s \n", str_res.c_str());

        fprintf(stdout, "cost time:%.2f ms \n", timer_postprocess.cost());

        fprintf(stdout, "--------------------------------------\n");
        auto total_time = std::accumulate(time_costs.begin(), time_costs.end(), 0.f);
        auto min_max_time = std::minmax_element(time_costs.begin(), time_costs.end());
        fprintf(stdout,
                "Repeat %d times, avg time %.2f ms, max_time %.2f ms, min_time %.2f ms\n",
                (int)time_costs.size(),
                total_time / (float)time_costs.size(),
                *min_max_time.second,
                *min_max_time.first);
    }

    bool run_model(const std::string &model, const std::vector<uint8_t> &data, const int &repeat, cv::Mat &mat, std::vector<std::string> &label_list_)
    {
        // 1. init engine
        AX_ENGINE_NPU_ATTR_T npu_attr;
        memset(&npu_attr, 0, sizeof(npu_attr));
        npu_attr.eHardMode = AX_ENGINE_VIRTUAL_NPU_DISABLE;
        auto ret = AX_ENGINE_Init(&npu_attr);
        if (0 != ret)
        {
            return ret;
        }

        // 2. load model
        std::vector<char> model_buffer;
        if (!utilities::read_file(model, model_buffer))
        {
            fprintf(stderr, "Read Run-Joint model(%s) file failed.\n", model.c_str());
            return false;
        }

        // 3. create handle
        AX_ENGINE_HANDLE handle;
        ret = AX_ENGINE_CreateHandle(&handle, model_buffer.data(), model_buffer.size());
        SAMPLE_AX_ENGINE_DEAL_HANDLE
        fprintf(stdout, "Engine creating handle is done.\n");

        // 4. create context
        ret = AX_ENGINE_CreateContext(handle);
        SAMPLE_AX_ENGINE_DEAL_HANDLE
        fprintf(stdout, "Engine creating context is done.\n");

        // 5. set io
        AX_ENGINE_IO_INFO_T *io_info;
        ret = AX_ENGINE_GetIOInfo(handle, &io_info);
        SAMPLE_AX_ENGINE_DEAL_HANDLE
        fprintf(stdout, "Engine get io info is done. \n");

        // 6. alloc io
        AX_ENGINE_IO_T io_data;
        ret = middleware::prepare_io(io_info, &io_data, std::make_pair(AX_ENGINE_ABST_DEFAULT, AX_ENGINE_ABST_CACHED));
        SAMPLE_AX_ENGINE_DEAL_HANDLE
        fprintf(stdout, "Engine alloc io is done. \n");

        // 7. insert input
        ret = middleware::push_input(data, &io_data, io_info);
        SAMPLE_AX_ENGINE_DEAL_HANDLE_IO
        fprintf(stdout, "Engine push input is done. \n");
        fprintf(stdout, "--------------------------------------\n");

        // 8. warn up
        for (int i = 0; i < 5; ++i)
        {
            AX_ENGINE_RunSync(handle, &io_data);
        }

        // 9. run model
        std::vector<float> time_costs(repeat, 0);
        for (int i = 0; i < repeat; ++i)
        {
            timer tick;
            ret = AX_ENGINE_RunSync(handle, &io_data);
            time_costs[i] = tick.cost();
            SAMPLE_AX_ENGINE_DEAL_HANDLE_IO
        }

        // 10. get result
        post_process(io_info, &io_data, mat, label_list_, time_costs);
        fprintf(stdout, "--------------------------------------\n");

        middleware::free_io(&io_data);
        return AX_ENGINE_DestroyHandle(handle);
    }
} // namespace ax

int main(int argc, char *argv[])
{
    cmdline::parser cmd;
    cmd.add<std::string>("model", 'm', "joint file(a.k.a. joint model)", true, "");
    cmd.add<std::string>("image", 'i', "image file", true, "");
    cmd.add<std::string>("dict", 'd', "dict file", true, "");
    cmd.add<std::string>("size", 'g', "input_h, input_w", false, std::to_string(DEFAULT_IMG_H) + "," + std::to_string(DEFAULT_IMG_W));

    cmd.add<int>("repeat", 'r', "repeat count", false, DEFAULT_LOOP_COUNT);
    cmd.parse_check(argc, argv);

    // 0. get app args, can be removed from user's app
    auto model_file = cmd.get<std::string>("model");
    auto image_file = cmd.get<std::string>("image");
    auto dict_file = cmd.get<std::string>("dict");

    auto model_file_flag = utilities::file_exist(model_file);
    auto image_file_flag = utilities::file_exist(image_file);

    if (!model_file_flag | !image_file_flag)
    {
        auto show_error = [](const std::string &kind, const std::string &value)
        {
            fprintf(stderr, "Input file %s(%s) is not exist, please check it.\n", kind.c_str(), value.c_str());
        };

        if (!model_file_flag)
        {
            show_error("model", model_file);
        }
        if (!image_file_flag)
        {
            show_error("image", image_file);
        }

        return -1;
    }

    auto input_size_string = cmd.get<std::string>("size");

    std::array<int, 2> input_size = {DEFAULT_IMG_H, DEFAULT_IMG_W};

    auto input_size_flag = utilities::parse_string(input_size_string, input_size);

    if (!input_size_flag)
    {
        auto show_error = [](const std::string &kind, const std::string &value)
        {
            fprintf(stderr, "Input %s(%s) is not allowed, please check it.\n", kind.c_str(), value.c_str());
        };

        show_error("size", input_size_string);

        return -1;
    }

    auto repeat = cmd.get<int>("repeat");

    // 1. print args
    fprintf(stdout, "--------------------------------------\n");
    fprintf(stdout, "model file : %s\n", model_file.c_str());
    fprintf(stdout, "image file : %s\n", image_file.c_str());
    fprintf(stdout, "dict file : %s\n", dict_file.c_str());
    fprintf(stdout, "img_h, img_w : %d %d\n", input_size[0], input_size[1]);
    fprintf(stdout, "--------------------------------------\n");

    // 2. read image & resize & transpose
    std::vector<uint8_t> image(input_size[0] * input_size[1] * 3, 0);
    cv::Mat mat = cv::imread(image_file);
    if (mat.empty())
    {
        fprintf(stderr, "Read image failed.\n");
        return -1;
    }
    {
        float height = mat.rows;
        float width = mat.cols;
        float ratio = height / DEFAULT_IMG_H;
        int new_width = width / ratio;
    }
    common::get_input_data_no_letterbox(mat, image, input_size[0], input_size[1]);

    std::vector<std::string> label_list_ = ax::ReadDict(dict_file);
    // 3. sys_init
    AX_SYS_Init();

    // 4. -  engine model  -  can only use AX_ENGINE** inside
    {
        // AX_ENGINE_NPUReset(); // todo ??
        ax::run_model(model_file, image, repeat, mat, label_list_);

        // 4.3 engine de init
        AX_ENGINE_Deinit();
        // AX_ENGINE_NPUReset();
    }
    // 4. -  engine model  -

    AX_SYS_Deinit();
    return 0;
}
