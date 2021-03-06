/*
 * Copyright 2019 NVIDIA Corporation
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *    http://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "MemoryInterfaces.hpp"
#include "NvEncoderCLIOptions.h"
#include "TC_CORE.hpp"
#include "Tasks.hpp"

#include <mutex>
#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <sstream>

using namespace std;
using namespace VPF;

namespace py = pybind11;

constexpr auto TASK_EXEC_SUCCESS = TaskExecStatus::TASK_EXEC_SUCCESS;
constexpr auto TASK_EXEC_FAIL = TaskExecStatus::TASK_EXEC_FAIL;

static auto ThrowOnCudaError = [](CUresult res, int lineNum = -1) {
  if (CUDA_SUCCESS != res) {
    stringstream ss;

    if (lineNum > 0) {
      ss << __FILE__ << ":";
      ss << lineNum << endl;
    }

    const char *errName = nullptr;
    if (CUDA_SUCCESS != cuGetErrorName(res, &errName)) {
      ss << "CUDA error with code " << res << endl;
    } else {
      ss << "CUDA error: " << errName << endl;
    }

    const char *errDesc = nullptr;
    if (CUDA_SUCCESS != cuGetErrorString(res, &errDesc)) {
      ss << "No error string available" << endl;
    } else {
      ss << errDesc << endl;
    }

    throw runtime_error(ss.str());
  }
};

class CudaResMgr {
  CudaResMgr() {
    ThrowOnCudaError(cuInit(0), __LINE__);

    int nGpu;
    ThrowOnCudaError(cuDeviceGetCount(&nGpu), __LINE__);

    for (int i = 0; i < nGpu; i++) {
      CUcontext cuContext = nullptr;
      CUstream cuStream = nullptr;

      g_Contexts.push_back(cuContext);
      g_Streams.push_back(cuStream);
    }
    return;
  }

public:
  static CudaResMgr &Instance() {
    static CudaResMgr instance;
    return instance;
  }

  CUcontext GetCtx(size_t idx) {
    if (idx >= GetNumGpus()) {
      return nullptr;
    }

    auto &ctx = g_Contexts[idx];
    if (!ctx) {
      CUdevice cuDevice = 0;
      ThrowOnCudaError(cuDeviceGet(&cuDevice, idx), __LINE__);
      ThrowOnCudaError(cuCtxCreate(&ctx, 0, cuDevice), __LINE__);
    }

    return g_Contexts[idx];
  }

  CUstream GetStream(size_t idx) {
    if (idx >= GetNumGpus()) {
      return nullptr;
    }

    auto &str = g_Streams[idx];
    if (!str) {
      auto ctx = GetCtx(idx);
      CudaCtxPush push(ctx);
      ThrowOnCudaError(cuStreamCreate(&str, 0), __LINE__);
    }

    return g_Streams[idx];
  }

  /* Also a static function as we want to keep all the
   * CUDA stuff within one Python module;
   */
  ~CudaResMgr() {
    stringstream ss;
    try {
      for (auto &cuStream : g_Streams) {
        if (cuStream) {
          ThrowOnCudaError(cuStreamDestroy(cuStream), __LINE__);
        }
      }
      g_Streams.clear();

      for (auto &cuContext : g_Contexts) {
        if (cuContext) {
          ThrowOnCudaError(cuCtxDestroy(cuContext), __LINE__);
        }
      }
      g_Contexts.clear();
    } catch (runtime_error &e) {
      cerr << e.what() << endl;
    }

#ifdef TRACK_TOKEN_ALLOCATIONS
    cout << "Checking token allocation counters: ";
    auto res = CheckAllocationCounters();
    cout << (res ? "No leaks dectected" : "Leaks detected") << endl;
#endif
  }

  static size_t GetNumGpus() { return Instance().g_Contexts.size(); }

  vector<CUcontext> g_Contexts;
  vector<CUstream> g_Streams;
};

class PyFrameUploader {
  unique_ptr<CudaUploadFrame> uploader;
  uint32_t gpuID = 0U, surfaceWidth, surfaceHeight;
  Pixel_Format surfaceFormat;

public:
  PyFrameUploader(uint32_t width, uint32_t height, Pixel_Format format,
                  uint32_t gpu_ID) {
    gpuID = gpu_ID;
    surfaceWidth = width;
    surfaceHeight = height;
    surfaceFormat = format;

    uploader.reset(
        CudaUploadFrame::Make(CudaResMgr::Instance().GetStream(gpuID),
                              CudaResMgr::Instance().GetCtx(gpuID),
                              surfaceWidth, surfaceHeight, surfaceFormat));
  }

  Pixel_Format GetFormat() { return surfaceFormat; }

  /* Will upload numpy array to GPU;
   * Surface returned is valid untill next call;
   */
  shared_ptr<Surface> UploadSingleFrame(py::array_t<uint8_t> &frame) {
    /* Upload to GPU;
     */
    auto pRawFrame = Buffer::Make(frame.size(), frame.mutable_data());
    uploader->SetInput(pRawFrame, 0U);
    auto res = uploader->Execute();
    delete pRawFrame;

    if (TASK_EXEC_FAIL == res) {
      throw runtime_error("Error uploading frame to GPU");
    }

    /* Get surface;
     */
    auto pSurface = (Surface *)uploader->GetOutput(0U);
    if (!pSurface) {
      throw runtime_error("Error uploading frame to GPU");
    }

    return shared_ptr<Surface>(pSurface->Clone());
  }
};

class PySurfaceDownloader {
  unique_ptr<CudaDownloadSurface> upDownloader;
  uint32_t gpuID = 0U, surfaceWidth, surfaceHeight;
  Pixel_Format surfaceFormat;

public:
  PySurfaceDownloader(uint32_t width, uint32_t height, Pixel_Format format,
                      uint32_t gpu_ID) {
    gpuID = gpu_ID;
    surfaceWidth = width;
    surfaceHeight = height;
    surfaceFormat = format;

    upDownloader.reset(
        CudaDownloadSurface::Make(CudaResMgr::Instance().GetStream(gpuID),
                                  CudaResMgr::Instance().GetCtx(gpuID),
                                  surfaceWidth, surfaceHeight, surfaceFormat));
  }

  Pixel_Format GetFormat() { return surfaceFormat; }

  bool DownloadSingleSurface(shared_ptr<Surface> surface,
                             py::array_t<uint8_t> &frame) {
    upDownloader->SetInput(surface.get(), 0U);
    if (TASK_EXEC_FAIL == upDownloader->Execute()) {
      return false;
    }

    auto *pRawFrame = (Buffer *)upDownloader->GetOutput(0U);
    if (pRawFrame) {
      auto const downloadSize = pRawFrame->GetRawMemSize();
      if (downloadSize != frame.size()) {
        frame.resize({downloadSize}, false);
      }

      memcpy(frame.mutable_data(), pRawFrame->GetRawMemPtr(), downloadSize);
      return true;
    }

    return false;
  }
};

class PySurfaceConverter {
  unique_ptr<ConvertSurface> upConverter;
  Pixel_Format outputFormat;
  uint32_t gpuId;

public:
  PySurfaceConverter(uint32_t width, uint32_t height, Pixel_Format inFormat,
                     Pixel_Format outFormat, uint32_t gpuID)
      : gpuId(gpuID), outputFormat(outFormat) {
    upConverter.reset(
        ConvertSurface::Make(width, height, inFormat, outFormat,
                             CudaResMgr::Instance().GetCtx(gpuId),
                             CudaResMgr::Instance().GetStream(gpuId)));
  }

  shared_ptr<Surface> Execute(shared_ptr<Surface> surface) {
    if (!surface) {
      return shared_ptr<Surface>(Surface::Make(outputFormat));
    }

    upConverter->SetInput(surface.get(), 0U);
    if (TASK_EXEC_SUCCESS != upConverter->Execute()) {
      return shared_ptr<Surface>(Surface::Make(outputFormat));
    }

    auto pSurface = (Surface *)upConverter->GetOutput(0U);
    return shared_ptr<Surface>(pSurface ? pSurface->Clone()
                                        : Surface::Make(outputFormat));
  }

  Pixel_Format GetFormat() { return outputFormat; }
};

class PySurfaceResizer {
  unique_ptr<ResizeSurface> upResizer;
  Pixel_Format outputFormat;
  uint32_t gpuId;

public:
  PySurfaceResizer(uint32_t width, uint32_t height, Pixel_Format format,
                   uint32_t gpuID)
      : outputFormat(format), gpuId(gpuID) {
    upResizer.reset(ResizeSurface::Make(
        width, height, format, CudaResMgr::Instance().GetCtx(gpuId),
        CudaResMgr::Instance().GetStream(gpuId)));
  }

  Pixel_Format GetFormat() { return outputFormat; }

  shared_ptr<Surface> Execute(shared_ptr<Surface> surface) {
    if (!surface) {
      return shared_ptr<Surface>(Surface::Make(outputFormat));
    }

    upResizer->SetInput(surface.get(), 0U);

    if (TASK_EXEC_SUCCESS != upResizer->Execute()) {
      return shared_ptr<Surface>(Surface::Make(outputFormat));
    }

    auto pSurface = (Surface *)upResizer->GetOutput(0U);
    return shared_ptr<Surface>(pSurface ? pSurface->Clone()
                                        : Surface::Make(outputFormat));
  }
};

class PyNvDecoder {
  unique_ptr<DemuxFrame> upDemuxer;
  unique_ptr<NvdecDecodeFrame> upDecoder;
  unique_ptr<PySurfaceDownloader> upDownloader;
  uint32_t gpuId;
  static uint32_t const poolFrameSize = 4U;

public:
  PyNvDecoder(const string &pathToFile, int gpuOrdinal)
      : PyNvDecoder(pathToFile, gpuOrdinal, map<string, string>()) {}

  PyNvDecoder(const string &pathToFile, int gpuOrdinal,
              const map<string, string> &ffmpeg_options) {
    if (gpuOrdinal < 0 || gpuOrdinal >= CudaResMgr::Instance().GetNumGpus()) {
      gpuOrdinal = 0U;
    }
    gpuId = gpuOrdinal;
    cout << "Decoding on GPU " << gpuId << endl;

    vector<const char *> options;
    for (auto &pair : ffmpeg_options) {
      options.push_back(pair.first.c_str());
      options.push_back(pair.second.c_str());
    }
    upDemuxer.reset(
        DemuxFrame::Make(pathToFile.c_str(), options.data(), options.size()));

    MuxingParams params;
    upDemuxer->GetParams(params);

    upDecoder.reset(NvdecDecodeFrame::Make(
        CudaResMgr::Instance().GetStream(gpuId),
        CudaResMgr::Instance().GetCtx(gpuId), params.videoContext.codec,
        poolFrameSize, params.videoContext.width, params.videoContext.height));
  }

  /* Extracts video elementary bitstream from input file;
   * Returns true in case of success, false otherwise;
   */
  static Buffer *getElementaryVideo(DemuxFrame *demuxer) {
    Buffer *elementaryVideo = nullptr;
    /* Demuxer may also extracts elementary audio etc. from stream, so we run it
     * until we get elementary video;
     */
    do {
      if (TASK_EXEC_FAIL == demuxer->Execute()) {
        return nullptr;
      }
      elementaryVideo = (Buffer *)demuxer->GetOutput(0U);
    } while (!elementaryVideo);

    return elementaryVideo;
  };

  /* Decodes single video sequence frame to surface in video memory;
   * Returns true in case of success, false otherwise;
   */
  static Surface *getDecodedSurface(NvdecDecodeFrame *decoder,
                                    DemuxFrame *demuxer) {
    Surface *surface = nullptr;
    do {
      /* Get encoded frame from demuxer;
       * May be null, but that's ok - it will flush decoder;
       */
      auto elementaryVideo = getElementaryVideo(demuxer);

      /* Kick off HW decoding;
       * We may not have decoded surface here as decoder is async;
       */
      decoder->SetInput(elementaryVideo, 0U);
      if (TASK_EXEC_FAIL == decoder->Execute()) {
        break;
      }

      surface = (Surface *)decoder->GetOutput(0U);
      /* Repeat untill we got decoded surface;
       */
    } while (!surface);

    return surface;
  };

  /* Feed decoder with empty input;
   * It will give single surface from decoded frames queue;
   * Returns true in case of success, false otherwise;
   */
  static bool getDecodedSurfaceFlush(NvdecDecodeFrame *decoder,
                                     DemuxFrame *demuxer, Surface *&output) {
    output = nullptr;
    auto *elementaryVideo = Buffer::Make(0U);
    decoder->SetInput(elementaryVideo, 0U);
    auto res = decoder->Execute();
    delete elementaryVideo;

    if (TASK_EXEC_FAIL == res) {
      return false;
    }

    output = (Surface *)decoder->GetOutput(0U);
    return output != nullptr;
  }

  uint32_t Width() const {
    MuxingParams params;
    upDemuxer->GetParams(params);
    return params.videoContext.width;
  }

  uint32_t Height() const {
    MuxingParams params;
    upDemuxer->GetParams(params);
    return params.videoContext.height;
  }

  uint32_t Framerate() const {
    MuxingParams params;
    upDemuxer->GetParams(params);
    return params.videoContext.frameRate;
  }

  uint32_t Framesize() const { return Width() * Height() * 3 / 2; }

  Pixel_Format GetPixelFormat() const {
    MuxingParams params;
    upDemuxer->GetParams(params);
    return params.videoContext.format;
  }

  /* Decodes single next frame from video to surface in video memory;
   * Returns shared ponter to surface class;
   * In case of failure, pointer to empty surface is returned;
   */
  shared_ptr<Surface> DecodeSingleSurface() {
    auto pRawSurf = getDecodedSurface(upDecoder.get(), upDemuxer.get());
    if (pRawSurf) {
      return shared_ptr<Surface>(pRawSurf->Clone());
    } else {
      auto pixFmt = GetPixelFormat();
      auto spSurface = shared_ptr<Surface>(Surface::Make(pixFmt));
      return spSurface;
    }
  }

  /* Decodes single next frame from video to numpy array;
   * In case of failure, empty array is returned;
   */
  bool DecodeSingleFrame(py::array_t<uint8_t> &frame) {
    auto spRawSufrace = DecodeSingleSurface();
    if (spRawSufrace->Empty()) {
      return false;
    }

    /* We init downloader here as now we know the exact decoded frame size;
     */
    if (!upDownloader) {
      uint32_t width, height, elem_size;
      upDecoder->GetDecodedFrameParams(width, height, elem_size);
      upDownloader.reset(new PySurfaceDownloader(width, height, NV12, gpuId));
    }

    return upDownloader->DownloadSingleSurface(spRawSufrace, frame);
  }
};

class PyNvEncoder {
  unique_ptr<PyFrameUploader> uploader;
  unique_ptr<NvencEncodeFrame> upEncoder;
  uint32_t encWidth, encHeight, gpuId;
  Pixel_Format eFormat = NV12;
  map<string, string> options;
  bool verbose_ctor;

public:
  uint32_t Width() const { return encWidth; }

  uint32_t Height() const { return encHeight; }

  Pixel_Format GetPixelFormat() const { return eFormat; }

  bool Reconfigure(const map<string, string> &encodeOptions,
                   bool force_idr = false, bool reset_enc = false,
                   bool verbose = false) {

    if (upEncoder) {
      NvEncoderClInterface cli_interface(encodeOptions);
      return upEncoder->Reconfigure(cli_interface, force_idr, reset_enc,
                                    verbose);
    }

    return true;
  }

  PyNvEncoder(const map<string, string> &encodeOptions, int gpuOrdinal,
              bool verbose = false)
      : upEncoder(nullptr), uploader(nullptr), options(encodeOptions),
        verbose_ctor(verbose) {

    auto ParseResolution = [&](const string &res_string, uint32_t &width,
                               uint32_t &height) {
      string::size_type xPos = res_string.find('x');

      if (xPos != string::npos) {
        // Parse width;
        stringstream ssWidth;
        ssWidth << res_string.substr(0, xPos);
        ssWidth >> width;

        // Parse height;
        stringstream ssHeight;
        ssHeight << res_string.substr(xPos + 1);
        ssHeight >> height;
      } else {
        throw invalid_argument("Invalid resolution.");
      }
    };

    auto it = options.find("s");
    if (it != options.end()) {
      ParseResolution(it->second, encWidth, encHeight);
    } else {
      throw invalid_argument("No resolution given");
    }

    if (gpuOrdinal < 0 || gpuOrdinal >= CudaResMgr::Instance().GetNumGpus()) {
      gpuOrdinal = 0U;
    }
    gpuId = gpuOrdinal;
    cout << "Encoding on GPU " << gpuId << endl;

    /* Don't initialize uploader & encoder here, ust prepare config params;
     */
    Reconfigure(options, false, false, verbose);
  }

  bool EncodeSurface(shared_ptr<Surface> rawSurface,
                     py::array_t<uint8_t> &packet, bool sync) {
    return EncodeSingleSurface(rawSurface, packet, false, sync);
  }

  bool EncodeSingleSurface(shared_ptr<Surface> rawSurface,
                           py::array_t<uint8_t> &packet, bool append,
                           bool sync) {
    if (!upEncoder) {
      NvEncoderClInterface cli_interface(options);

      upEncoder.reset(NvencEncodeFrame::Make(
          CudaResMgr::Instance().GetStream(gpuId),
          CudaResMgr::Instance().GetCtx(gpuId), cli_interface,
          NV_ENC_BUFFER_FORMAT_NV12, encWidth, encHeight, verbose_ctor));
    }

    upEncoder->ClearInputs();

    if (rawSurface) {
      upEncoder->SetInput(rawSurface.get(), 0U);
    } else {
      /* Flush encoder this way;
       */
      upEncoder->SetInput(nullptr, 0U);
    }

    if (sync) {
      /* Set 2nd input to any non-zero value 
       * to signal sync encode;
       */
      upEncoder->SetInput((Token *)0xdeadbeef, 1U);
    }

    if (TASK_EXEC_FAIL == upEncoder->Execute()) {
      throw runtime_error("Error while encoding frame");
    }

    auto encodedFrame = (Buffer *)upEncoder->GetOutput(0U);
    if (encodedFrame) {
      if (append) {
        auto old_size = packet.size();
        packet.resize({old_size + encodedFrame->GetRawMemSize()}, false);
        memcpy(packet.mutable_data() + old_size, encodedFrame->GetRawMemPtr(),
               encodedFrame->GetRawMemSize());
      } else {
        packet.resize({encodedFrame->GetRawMemSize()}, false);
        memcpy(packet.mutable_data(), encodedFrame->GetRawMemPtr(),
               encodedFrame->GetRawMemSize());
      }
      return true;
    }

    return false;
  }

  bool EncodeSingleFrame(py::array_t<uint8_t> &inRawFrame,
                         py::array_t<uint8_t> &packet, bool sync) {
    if (!uploader) {
      uploader.reset(new PyFrameUploader(encWidth, encHeight, eFormat, gpuId));
    }

    return EncodeSingleSurface(uploader->UploadSingleFrame(inRawFrame), packet,
                               false, sync);
  }

  bool Flush(py::array_t<uint8_t> &packets) {
    uint32_t num_packets = 0U;
    do {
      /* Keep feeding encoder with null input until it returns zero-size
       * surface; */
      auto success = EncodeSingleSurface(nullptr, packets, true, false);
      if (!success) {
        break;
      }
      num_packets++;
    } while (true);

    return (num_packets > 0U);
  }
};

auto CopySurface = [](shared_ptr<Surface> self, shared_ptr<Surface> other,
                      int gpuID) {
  auto cudaCtx = CudaResMgr::Instance().GetCtx(gpuID);
  CUstream cudaStream = CudaResMgr::Instance().GetStream(gpuID);

  for (auto plane = 0U; plane < self->NumPlanes(); plane++) {
    auto srcPlanePtr = self->PlanePtr(plane);
    auto dstPlanePtr = other->PlanePtr(plane);

    if (!srcPlanePtr || !dstPlanePtr) {
      break;
    }

    CudaCtxPush ctxPush(cudaCtx);

    CUDA_MEMCPY2D m = {0};
    m.srcMemoryType = CU_MEMORYTYPE_DEVICE;
    m.dstMemoryType = CU_MEMORYTYPE_DEVICE;
    m.srcDevice = srcPlanePtr;
    m.dstDevice = dstPlanePtr;
    m.srcPitch = self->Pitch(plane);
    m.dstPitch = other->Pitch(plane);
    m.Height = self->Height(plane);
    m.WidthInBytes = self->WidthInBytes(plane);

    ThrowOnCudaError(cuMemcpy2DAsync(&m, cudaStream), __LINE__);
  }

  ThrowOnCudaError(cuStreamSynchronize(cudaStream), __LINE__);
};

PYBIND11_MODULE(PyNvCodec, m) {
  m.doc() = "Python bindings for Nvidia-accelerated video processing";

  py::enum_<Pixel_Format>(m, "PixelFormat")
      .value("Y", Pixel_Format::Y)
      .value("RGB", Pixel_Format::RGB)
      .value("NV12", Pixel_Format::NV12)
      .value("YUV420", Pixel_Format::YUV420)
      .value("RGB_PLANAR", Pixel_Format::RGB_PLANAR)
      .value("UNDEFINED", Pixel_Format::UNDEFINED)
      .export_values();

  py::class_<SurfacePlane, shared_ptr<SurfacePlane>>(m, "SurfacePlane")
      .def("Width", &SurfacePlane::Width)
      .def("Height", &SurfacePlane::Height)
      .def("Pitch", &SurfacePlane::Pitch)
      .def("GpuMem", &SurfacePlane::GpuMem)
      .def("ElemSize", &SurfacePlane::ElemSize)
      .def("HostFrameSize", &SurfacePlane::GetHostMemSize);

  py::class_<Surface, shared_ptr<Surface>>(m, "Surface")
      .def("Width", &Surface::Width, py::arg("planeNumber") = 0U)
      .def("Height", &Surface::Height, py::arg("planeNumber") = 0U)
      .def("Pitch", &Surface::Pitch, py::arg("planeNumber") = 0U)
      .def("Format", &Surface::PixelFormat)
      .def("Empty", &Surface::Empty)
      .def("NumPlanes", &Surface::NumPlanes)
      .def("HostSize", &Surface::HostMemSize)
      .def_static("Make",
                  [](Pixel_Format format, uint32_t newWidth, uint32_t newHeight,
                     int gpuID) {
                    auto pNewSurf = shared_ptr<Surface>(
                        Surface::Make(format, newWidth, newHeight,
                                      CudaResMgr::Instance().GetCtx(gpuID)));
                    return pNewSurf;
                  },
                  py::return_value_policy::take_ownership)
      .def("PlanePtr",
           [](shared_ptr<Surface> self, int planeNumber) {
             auto pPlane = self->GetSurfacePlane(planeNumber);
             return make_shared<SurfacePlane>(*pPlane);
           },
           // Integral part of Surface, only reference it;
           py::arg("planeNumber") = 0U, py::return_value_policy::reference)
      .def("CopyFrom",
           [](shared_ptr<Surface> self, shared_ptr<Surface> other, int gpuID) {
             if (self->PixelFormat() != other->PixelFormat()) {
               throw runtime_error("Surfaces have different pixel formats");
             }

             if (self->Width() != other->Width() ||
                 self->Height() != other->Height()) {
               throw runtime_error("Surfaces have different size");
             }

             CopySurface(self, other, gpuID);
           })
      .def("Clone",
           [](shared_ptr<Surface> self, int gpuID) {
             auto pNewSurf = shared_ptr<Surface>(Surface::Make(
                 self->PixelFormat(), self->Width(), self->Height(),
                 CudaResMgr::Instance().GetCtx(gpuID)));

             CopySurface(self, pNewSurf, gpuID);
             return pNewSurf;
           },
           py::return_value_policy::take_ownership);

  py::class_<PyNvEncoder>(m, "PyNvEncoder")
      .def(py::init<const map<string, string> &, int, bool>(),
           py::arg("settings"), py::arg("gpu_id"), py::arg("verbose") = false)
      .def("Reconfigure", &PyNvEncoder::Reconfigure, py::arg("settings"),
           py::arg("force_idr") = false, py::arg("reset_encoder") = false,
           py::arg("verbose") = false)
      .def("Width", &PyNvEncoder::Width)
      .def("Height", &PyNvEncoder::Height)
      .def("Format", &PyNvEncoder::GetPixelFormat)
      .def("EncodeSingleSurface", &PyNvEncoder::EncodeSurface,
           py::arg("surface"), py::arg("packet"), py::arg("sync") = false)
      .def("EncodeSingleFrame", &PyNvEncoder::EncodeSingleFrame,
           py::arg("frame"), py::arg("packet"), py::arg("sync") = false)
      .def("Flush", &PyNvEncoder::Flush);

  py::class_<PyNvDecoder>(m, "PyNvDecoder")
      .def(py::init<const string &, int, const map<string, string> &>())
      .def(py::init<const string &, int>())
      .def("Width", &PyNvDecoder::Width)
      .def("Height", &PyNvDecoder::Height)
      .def("Framerate", &PyNvDecoder::Framerate)
      .def("Framesize", &PyNvDecoder::Framesize)
      .def("Format", &PyNvDecoder::GetPixelFormat)
      .def("DecodeSingleSurface", &PyNvDecoder::DecodeSingleSurface,
           py::return_value_policy::take_ownership)
      .def("DecodeSingleFrame", &PyNvDecoder::DecodeSingleFrame);

  py::class_<PyFrameUploader>(m, "PyFrameUploader")
      .def(py::init<uint32_t, uint32_t, Pixel_Format, uint32_t>())
      .def("Format", &PyFrameUploader::GetFormat)
      .def("UploadSingleFrame", &PyFrameUploader::UploadSingleFrame,
           py::return_value_policy::take_ownership);

  py::class_<PySurfaceDownloader>(m, "PySurfaceDownloader")
      .def(py::init<uint32_t, uint32_t, Pixel_Format, uint32_t>())
      .def("Format", &PySurfaceDownloader::GetFormat)
      .def("DownloadSingleSurface",
           &PySurfaceDownloader::DownloadSingleSurface);

  py::class_<PySurfaceConverter>(m, "PySurfaceConverter")
      .def(py::init<uint32_t, uint32_t, Pixel_Format, Pixel_Format, uint32_t>())
      .def("Format", &PySurfaceConverter::GetFormat)
      .def("Execute", &PySurfaceConverter::Execute,
           py::return_value_policy::take_ownership);

  py::class_<PySurfaceResizer>(m, "PySurfaceResizer")
      .def(py::init<uint32_t, uint32_t, Pixel_Format, uint32_t>())
      .def("Format", &PySurfaceResizer::GetFormat)
      .def("Execute", &PySurfaceResizer::Execute,
           py::return_value_policy::take_ownership);

  m.def("GetNumGpus", &CudaResMgr::GetNumGpus);
}
