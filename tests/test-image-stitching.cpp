/*
 * test-image-stitching.cpp - test image stitching
 *
 *  Copyright (c) 2016 Intel Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Author: Yinhang Liu <yinhangx.liu@intel.com>
 * Author: Wind Yuan <feng.yuan@intel.com>
 */

#include "test_common.h"
#include "test_inline.h"
#include <unistd.h>
#include <getopt.h>
#include "ocl/cl_device.h"
#include "ocl/cl_context.h"
#include "drm_display.h"
#include "image_file_handle.h"
#include "ocl/cl_fisheye_handler.h"
#include "ocl/cl_image_360_stitch.h"

#define XCAM_TEST_STITCH_DEBUG 0
#define XCAM_ALIGNED_WIDTH 16

using namespace XCam;

#if HAVE_OPENCV
#if XCAM_TEST_STITCH_DEBUG
static void dbg_write_image (
    SmartPtr<CLContext> context, SmartPtr<CLImage360Stitch> image_360,
    SmartPtr<VideoBuffer> input_bufs[], SmartPtr<VideoBuffer> output_buf,
    bool all_in_one, int fisheye_num, int input_count);
#endif

void
init_opencv_ocl (SmartPtr<CLContext> context)
{
    cl_platform_id platform_id = CLDevice::instance()->get_platform_id ();
    char *platform_name = CLDevice::instance()->get_platform_name ();
    cl_device_id device_id = CLDevice::instance()->get_device_id ();
    cl_context context_id = context->get_context_id ();
    cv::ocl::attachContext (platform_name, platform_id, context_id, device_id);
}

bool
convert_to_mat (SmartPtr<CLContext> context, SmartPtr<VideoBuffer> buffer, cv::Mat &image)
{
    SmartPtr<CLBuffer> cl_buffer = convert_to_clbuffer (context, buffer);
    VideoBufferInfo info = buffer->get_video_info ();
    cl_mem cl_mem_id = cl_buffer->get_mem_id ();

    cv::UMat umat;
    cv::ocl::convertFromBuffer (cl_mem_id, info.strides[0], info.height * 3 / 2, info.width, CV_8U, umat);
    if (umat.empty ()) {
        XCAM_LOG_ERROR ("convert bo buffer to UMat failed");
        return false;
    }

    cv::Mat mat;
    umat.copyTo (mat);
    if (mat.empty ()) {
        XCAM_LOG_ERROR ("copy UMat to Mat failed");
        return false;
    }

    cv::cvtColor (mat, image, cv::COLOR_YUV2BGR_NV12);
    return true;
}
#endif

void usage(const char* arg0)
{
    printf ("Usage:\n"
            "%s --input file --output file\n"
            "\t--input             input image(NV12)\n"
            "\t--output            output image(NV12)\n"
            "\t--input-w           optional, input width, default: 1920\n"
            "\t--input-h           optional, input height, default: 1080\n"
            "\t--output-w          optional, output width, default: 1920\n"
            "\t--output-h          optional, output width, default: 960\n"
            "\t--res-mode          optional, image resolution mode, select from [1080p/1080p4/4k], default: 1080p\n"
            "\t--scale-mode        optional, image scaling mode, select from [local/global], default: local\n"
            "\t--enable-seam       optional, enable seam finder in blending area, default: no\n"
            "\t--enable-fisheyemap optional, enable fisheye map, default: no\n"
            "\t--enable-lsc        optional, enable lens shading correction, default: no\n"
#if HAVE_OPENCV
            "\t--fm-ocl            optional, enable ocl for feature match, select from [true/false], default: false\n"
#endif
            "\t--fisheye-num       optional, the number of fisheye lens, default: 2\n"
            "\t--all-in-one        optional, all fisheye in one image, select from [true/false], default: true\n"
            "\t--save              optional, save file or not, select from [true/false], default: true\n"
            "\t--framerate         optional, framerate of saved video, default: 30.0\n"
            "\t--loop              optional, how many loops need to run for performance test, default: 1\n"
            "\t--help              usage\n",
            arg0);
}

int main (int argc, char *argv[])
{
    XCamReturn ret = XCAM_RETURN_NO_ERROR;
    SmartPtr<CLContext> context;
    SmartPtr<BufferPool> buf_pool[XCAM_STITCH_FISHEYE_MAX_NUM];
    ImageFileHandle file_in[XCAM_STITCH_FISHEYE_MAX_NUM];
    ImageFileHandle file_out;
    SmartPtr<VideoBuffer> input_buf, output_buf;
    VideoBufferInfo input_buf_info, output_buf_info;
    SmartPtr<CLImage360Stitch> image_360;

    uint32_t input_format = V4L2_PIX_FMT_NV12;
    uint32_t input_width = 1920;
    uint32_t input_height = 1080;
    uint32_t output_height = 960;
    uint32_t output_width = output_height * 2;

    int loop = 1;
    bool enable_seam = false;
    bool enable_fisheye_map = false;
    bool enable_lsc = false;
    CLBlenderScaleMode scale_mode = CLBlenderScaleLocal;
    StitchResMode res_mode = StitchRes1080P;

#if HAVE_OPENCV
    bool fm_ocl = false;
#endif
    int fisheye_num = 2;
    bool all_in_one = true;
    bool need_save_output = true;
    double framerate = 30.0;

    const char *file_in_name[XCAM_STITCH_FISHEYE_MAX_NUM] = {NULL};
    const char *file_out_name = NULL;

    int input_count = 0;

    const struct option long_opts[] = {
        {"input", required_argument, NULL, 'i'},
        {"output", required_argument, NULL, 'o'},
        {"input-w", required_argument, NULL, 'w'},
        {"input-h", required_argument, NULL, 'h'},
        {"output-w", required_argument, NULL, 'W'},
        {"output-h", required_argument, NULL, 'H'},
        {"res-mode", required_argument, NULL, 'R'},
        {"scale-mode", required_argument, NULL, 'c'},
        {"enable-seam", no_argument, NULL, 'S'},
        {"enable-fisheyemap", no_argument, NULL, 'F'},
        {"enable-lsc", no_argument, NULL, 'L'},
#if HAVE_OPENCV
        {"fm-ocl", required_argument, NULL, 'O'},
#endif
        {"fisheye-num", required_argument, NULL, 'N'},
        {"all-in-one", required_argument, NULL, 'A'},
        {"save", required_argument, NULL, 's'},
        {"framerate", required_argument, NULL, 'f'},
        {"loop", required_argument, NULL, 'l'},
        {"help", no_argument, NULL, 'e'},
        {NULL, 0, NULL, 0},
    };

    int opt = -1;
    while ((opt = getopt_long(argc, argv, "", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'i':
            XCAM_ASSERT (optarg);
            file_in_name[input_count] = optarg;
            input_count++;
            break;
        case 'o':
            XCAM_ASSERT (optarg);
            file_out_name = optarg;
            break;
        case 'w':
            input_width = atoi(optarg);
            break;
        case 'h':
            input_height = atoi(optarg);
            break;
        case 'W':
            output_width = atoi(optarg);
            break;
        case 'H':
            output_height = atoi(optarg);
            break;
        case 'R':
            if (!strcasecmp (optarg, "1080p"))
                res_mode = StitchRes1080P;
            else if (!strcasecmp (optarg, "1080p4"))
                res_mode = StitchRes1080P4;
            else if (!strcasecmp (optarg, "4k"))
                res_mode = StitchRes4K;
            else {
                XCAM_LOG_ERROR ("incorrect resolution mode");
                return -1;
            }
            break;
        case 'c':
            if (!strcasecmp (optarg, "local"))
                scale_mode = CLBlenderScaleLocal;
            else if (!strcasecmp (optarg, "global"))
                scale_mode = CLBlenderScaleGlobal;
            else {
                XCAM_LOG_ERROR ("incorrect scaling mode");
                return -1;
            }
            break;
        case 'S':
            enable_seam = true;
            break;
        case 'F':
            enable_fisheye_map = true;
            break;
        case 'L':
            enable_lsc = true;
            break;
#if HAVE_OPENCV
        case 'O':
            fm_ocl = (strcasecmp (optarg, "true") == 0 ? true : false);
            break;
#endif
        case 'N':
            fisheye_num = atoi(optarg);
            if (fisheye_num > XCAM_STITCH_FISHEYE_MAX_NUM) {
                XCAM_LOG_ERROR ("fisheye number should not be greater than %d\n", XCAM_STITCH_FISHEYE_MAX_NUM);
                return -1;
            }
            break;
        case 'A':
            all_in_one = (strcasecmp (optarg, "false") == 0 ? false : true);
            break;
        case 's':
            need_save_output = (strcasecmp (optarg, "false") == 0 ? false : true);
            break;
        case 'f':
            framerate = atof(optarg);
            break;
        case 'l':
            loop = atoi(optarg);
            break;
        case 'e':
            usage (argv[0]);
            return -1;
        default:
            XCAM_LOG_ERROR ("getopt_long return unknown value:%c", opt);
            usage (argv[0]);
            return -1;
        }
    }

    if (optind < argc || argc < 2) {
        XCAM_LOG_ERROR ("unknown option %s", argv[optind]);
        usage (argv[0]);
        return -1;
    }

    if (!all_in_one && input_count != fisheye_num) {
        XCAM_LOG_ERROR ("multiple-input mode: conflicting input number(%d) and fisheye number(%d)",
                        input_count, fisheye_num);
        return -1;
    }

    for (int i = 0; i < input_count; i++) {
        if (!file_in_name[i]) {
            XCAM_LOG_ERROR ("input[%d] path is NULL", i);
            return -1;
        }
    }

    if (!file_out_name) {
        XCAM_LOG_ERROR ("output path is NULL");
        return -1;
    }

    output_width = XCAM_ALIGN_UP (output_width, XCAM_ALIGNED_WIDTH);
    output_height = XCAM_ALIGN_UP (output_height, XCAM_ALIGNED_WIDTH);
    // if (output_width != output_height * 2) {
    //     XCAM_LOG_ERROR ("incorrect output size width:%d height:%d", output_width, output_height);
    //     return -1;
    // }

#if !HAVE_OPENCV
    if (need_save_output) {
        XCAM_LOG_WARNING ("non-OpenCV mode, can't save video");
        need_save_output = false;
    }
#endif

    printf ("Description------------------------\n");
    if (all_in_one)
        printf ("input file:\t\t%s\n", file_in_name[0]);
    else {
        for (int i = 0; i < input_count; i++)
            printf ("input file %d:\t\t%s\n", i, file_in_name[i]);
    }
    printf ("output file:\t\t%s\n", file_out_name);
    printf ("input width:\t\t%d\n", input_width);
    printf ("input height:\t\t%d\n", input_height);
    printf ("output width:\t\t%d\n", output_width);
    printf ("output height:\t\t%d\n", output_height);
    printf ("resolution mode:\t%s\n",
            res_mode == StitchRes1080P ? "1080P" : (res_mode == StitchRes1080P4 ? "1080P4" : "4K"));
    printf ("scale mode:\t\t%s\n", scale_mode == CLBlenderScaleLocal ? "local" : "global");
    printf ("seam mask:\t\t%s\n", enable_seam ? "true" : "false");
    printf ("fisheye map:\t\t%s\n", enable_fisheye_map ? "true" : "false");
    printf ("shading correction:\t%s\n", enable_lsc ? "true" : "false");
#if HAVE_OPENCV
    printf ("feature match ocl:\t%s\n", fm_ocl ? "true" : "false");
#endif
    printf ("fisheye number:\t\t%d\n", fisheye_num);
    printf ("all in one:\t\t%s\n", all_in_one ? "true" : "false");
    printf ("save file:\t\t%s\n", need_save_output ? "true" : "false");
    printf ("framerate:\t\t%.3lf\n", framerate);
    printf ("loop count:\t\t%d\n", loop);
    printf ("-----------------------------------\n");

    context = CLDevice::instance ()->get_context ();
    image_360 =
        create_image_360_stitch (
            context, enable_seam, scale_mode,
            enable_fisheye_map, enable_lsc, res_mode, fisheye_num, all_in_one).dynamic_cast_ptr<CLImage360Stitch> ();
    XCAM_ASSERT (image_360.ptr ());
    image_360->set_output_size (output_width, output_height);
#if HAVE_OPENCV
    image_360->set_feature_match_ocl (fm_ocl);
#endif
    image_360->set_pool_type (CLImageHandler::CLVideoPoolType);

    input_buf_info.init (input_format, input_width, input_height);
    output_buf_info.init (input_format, output_width, output_height);
    for (int i = 0; i < input_count; i++) {
        buf_pool[i] = new CLVideoBufferPool ();
        XCAM_ASSERT (buf_pool[i].ptr ());
        buf_pool[i]->set_video_info (input_buf_info);
        if (!buf_pool[i]->reserve (6)) {
            XCAM_LOG_ERROR ("init buffer pool failed");
            return -1;
        }
    }

    for (int i = 0; i < input_count; i++) {
        ret = file_in[i].open (file_in_name[i], "rb");
        CHECK (ret, "open %s failed", file_in_name[i]);
    }

#if HAVE_OPENCV
    init_opencv_ocl (context);

    cv::VideoWriter writer;
    if (need_save_output) {
        cv::Size dst_size = cv::Size (output_width, output_height);
        if (!writer.open (file_out_name, CV_FOURCC('X', '2', '6', '4'), framerate, dst_size)) {
            XCAM_LOG_ERROR ("open file %s failed", file_out_name);
            return -1;
        }
    }
#endif

    SmartPtr<VideoBuffer> pre_buf, cur_buf;
#if (HAVE_OPENCV) && (XCAM_TEST_STITCH_DEBUG)
    SmartPtr<VideoBuffer> input_bufs[XCAM_STITCH_FISHEYE_MAX_NUM];
#endif
    while (loop--) {
        for (int i = 0; i < input_count; i++) {
            ret = file_in[i].rewind ();
            CHECK (ret, "image_360 stitch rewind file(%s) failed", file_in_name[i]);
        }

        do {
            for (int i = 0; i < input_count; i++) {
                cur_buf = buf_pool[i]->get_buffer (buf_pool[i]);
                XCAM_ASSERT (cur_buf.ptr ());
                ret = file_in[i].read_buf (cur_buf);
                if (ret == XCAM_RETURN_BYPASS)
                    break;
                if (ret == XCAM_RETURN_ERROR_FILE) {
                    XCAM_LOG_ERROR ("read buffer from %s failed", file_in_name[i]);
                    return -1;
                }

                if (i == 0)
                    input_buf = cur_buf;
                else
                    pre_buf->attach_buffer (cur_buf);

                pre_buf = cur_buf;
#if (HAVE_OPENCV) && (XCAM_TEST_STITCH_DEBUG)
                input_bufs[i] = cur_buf;
#endif
            }
            if (ret == XCAM_RETURN_BYPASS)
                break;

            ret = image_360->execute (input_buf, output_buf);
            CHECK (ret, "image_360 stitch execute failed");

#if HAVE_OPENCV
            if (need_save_output) {
                cv::Mat out_mat;
                convert_to_mat (context, output_buf, out_mat);
                writer.write (out_mat);

#if XCAM_TEST_STITCH_DEBUG
                dbg_write_image (context, image_360, input_bufs, output_buf, all_in_one, fisheye_num, input_count);
#endif
            } else
#endif
                ensure_gpu_buffer_done (output_buf);

            FPS_CALCULATION (image_stitching, XCAM_OBJ_DUR_FRAME_NUM);
        } while (true);
    }

    return 0;
}

#if (HAVE_OPENCV) && (XCAM_TEST_STITCH_DEBUG)
static void dbg_write_image (
    SmartPtr<CLContext> context, SmartPtr<CLImage360Stitch> image_360,
    SmartPtr<VideoBuffer> input_bufs[], SmartPtr<VideoBuffer> output_buf,
    bool all_in_one, int fisheye_num, int input_count)
{
    cv::Mat mat;
    static int frame_count = 0;
    char file_name [1024];
    StitchInfo stitch_info = image_360->get_stitch_info ();

    std::snprintf (file_name, 1023, "orig_fisheye_%d.jpg", frame_count);
    for (int i = 0; i < input_count; i++) {
        if (!all_in_one)
            std::snprintf (file_name, 1023, "orig_fisheye_%d_%d.jpg", frame_count, i);

        convert_to_mat (context, input_bufs[i], mat);
        int fisheye_per_frame = all_in_one ? fisheye_num : 1;
        for (int i = 0; i < fisheye_per_frame; i++) {
            cv::circle (mat, cv::Point(stitch_info.fisheye_info[i].center_x, stitch_info.fisheye_info[i].center_y),
                        stitch_info.fisheye_info[i].radius, cv::Scalar(0, 0, 255), 2);
        }
        cv::imwrite (file_name, mat);
    }

    char frame_str[1024];
    std::snprintf (frame_str, 1023, "%d", frame_count);

    convert_to_mat (context, output_buf, mat);
    cv::putText (mat, frame_str, cv::Point(120, 120), cv::FONT_HERSHEY_COMPLEX, 2.0,
                 cv::Scalar(0, 0, 255), 2, 8, false);
    std::snprintf (file_name, 1023, "stitched_img_%d.jpg", frame_count);
    cv::imwrite (file_name, mat);

    frame_count++;
}
#endif

