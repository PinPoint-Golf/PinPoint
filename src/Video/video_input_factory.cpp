/*
 * Copyright (c) 2026 Mark Liversedge (liversedge@gmail.com)
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "video_input_factory.h"
#include "video_input.h"
#include "camera_capabilities.h"
#include "../Core/device_enumerator.h"

#ifdef Q_OS_MACOS
#include "VideoInputApple.h"
#include <QCameraDevice>
#include <QMediaDevices>
#endif

#ifdef HAVE_SPINNAKER
#include "SpinnakerPlatform.h"
#undef SPINNAKER_DEPRECATED_CLASS
#define SPINNAKER_DEPRECATED_CLASS(msg) class SPINNAKER_API __declspec(deprecated(msg))
#include <Spinnaker.h>
#include "VideoInputSpinnaker.h"
#include "spinnaker_runtime.h"
#endif

#ifdef HAVE_PPCP
#include "VideoInputPpcp.h"
#endif

#ifdef HAVE_ARAVIS
#include "VideoInputAravis.h"
// Include Aravis headers last to avoid 'signals' conflict
#undef signals
#include <arv.h>
#define signals public
#endif

#include "pp_debug.h"

void VideoInputFactory::enumerateDevices()
{
#ifdef Q_OS_MACOS
    // macOS: capabilities come from QCameraDevice — no connection needed.
    for (const QCameraDevice &dev : QMediaDevices::videoInputs()) {
        DeviceEnumerator::instance()->registerDevice(
            DeviceType::VideoInput, Backend::AppleAVFoundation,
            dev.id(), dev.description(),
            VideoInput::capabilitiesFor(dev));
    }
#else
    // Qt Multimedia: VideoInput::availableDevices() now passes capabilities.
    VideoInput::availableDevices();
#endif

#ifdef HAVE_ARAVIS
    // Aravis: open each device briefly to read its GenICam parameters, then
    // release — capabilities are stored in the Device struct so CameraInstance
    // never needs to re-open a camera just for format discovery.
    arv_update_device_list();
    unsigned int nAravis = arv_get_n_devices();
    for (unsigned int i = 0; i < nAravis; ++i) {
        const char *deviceId = arv_get_device_id(i);
        const char *model    = arv_get_device_model(i);

        CameraCapabilities caps;
        caps.driverVersion = "Aravis GigE/USB3 Vision";
        GError *err = nullptr;
        ArvCamera *cam = arv_camera_new(deviceId, &err);
        if (cam) {
            caps.connectionInterface = arv_camera_is_gv_device(cam)
                ? CameraCapabilities::Interface::GigE
                : CameraCapabilities::Interface::USB3;

            ArvDevice *arvDev = arv_camera_get_device(cam);
            if (arvDev) {
                GError *snErr = nullptr;
                const char *sn = arv_device_get_string_feature_value(arvDev, "DeviceSerialNumber", &snErr);
                if (sn && !snErr) caps.serialNumber = QString::fromLocal8Bit(sn);
                g_clear_error(&snErr);
                GError *vnErr = nullptr;
                const char *vn = arv_device_get_string_feature_value(arvDev, "DeviceVendorName", &vnErr);
                if (vn && !vnErr) caps.vendorName = QString::fromLocal8Bit(vn);
                g_clear_error(&vnErr);
            }
            caps.modelName = QString::fromLocal8Bit(model);

            gint curW = 0, curH = 0;
            arv_camera_get_region(cam, nullptr, nullptr, &curW, &curH, nullptr);
            gint wMin, wMax, hMin, hMax, wInc, hInc;
            arv_camera_get_width_bounds(cam, &wMin, &wMax, nullptr);
            arv_camera_get_height_bounds(cam, &hMin, &hMax, nullptr);
            wInc = arv_camera_get_width_increment(cam, nullptr);
            hInc = arv_camera_get_height_increment(cam, nullptr);
            // Width/Height bounds are offset-dependent (max = WidthMax -
            // OffsetX) and the camera retains the last ROI across
            // connections — prefer the WidthMax/HeightMax features, which
            // always report the full (binned) sensor.
            if (arvDev) {
                GError *mErr = nullptr;
                gint64 wm = arv_device_get_integer_feature_value(arvDev, "WidthMax", &mErr);
                if (!mErr && wm > 0) wMax = (gint)wm;
                g_clear_error(&mErr);
                gint64 hm = arv_device_get_integer_feature_value(arvDev, "HeightMax", &mErr);
                if (!mErr && hm > 0) hMax = (gint)hm;
                g_clear_error(&mErr);
            }
            caps.resolution.kind     = CapabilityKind::Range;
            caps.resolution.writable = true;
            caps.resolution.widthRange  = { wMin, wMax, wInc, curW };
            caps.resolution.heightRange = { hMin, hMax, hInc, curH };
            caps.resolution.defaultResolution = { curW, curH };

            // Hardware ROI via arv_camera_set_region (applied in start()).
            caps.roi.supported    = true;
            caps.roi.widthRange   = caps.resolution.widthRange;
            caps.roi.heightRange  = caps.resolution.heightRange;
            caps.roi.offsetXRange = { 0, wMax, wInc, 0 };
            caps.roi.offsetYRange = { 0, hMax, hInc, 0 };

            guint nFormats = 0;
            const char **fmts = arv_camera_dup_available_pixel_formats_as_strings(cam, &nFormats, nullptr);
            if (fmts) {
                caps.pixelFormat.kind     = CapabilityKind::Discrete;
                caps.pixelFormat.writable = true;
                const char *curFmt = arv_camera_get_pixel_format_as_string(cam, nullptr);
                for (guint j = 0; j < nFormats; ++j) {
                    PixelFormat pf;
                    pf.nativeKey = QString::fromLocal8Bit(fmts[j]);
                    if      (pf.nativeKey == "BayerRG8")   { pf.encoding = PixelEncoding::BayerRG8;  pf.bitsPerPixel = 8; }
                    else if (pf.nativeKey == "BayerGB8")   { pf.encoding = PixelEncoding::BayerGB8;  pf.bitsPerPixel = 8; }
                    else if (pf.nativeKey == "BayerGR8")   { pf.encoding = PixelEncoding::BayerGR8;  pf.bitsPerPixel = 8; }
                    else if (pf.nativeKey == "BayerBG8")   { pf.encoding = PixelEncoding::BayerBG8;  pf.bitsPerPixel = 8; }
                    else if (pf.nativeKey == "Mono8")      { pf.encoding = PixelEncoding::Mono8;     pf.bitsPerPixel = 8; }
                    else if (pf.nativeKey == "Mono10")     { pf.encoding = PixelEncoding::Mono10;    pf.bitsPerPixel = 10; }
                    else if (pf.nativeKey == "Mono12")     { pf.encoding = PixelEncoding::Mono12;    pf.bitsPerPixel = 12; }
                    else if (pf.nativeKey == "RGB8Packed") { pf.encoding = PixelEncoding::RGB8;      pf.bitsPerPixel = 24; }
                    else if (pf.nativeKey == "BGR8")       { pf.encoding = PixelEncoding::BGR8;      pf.bitsPerPixel = 24; }
                    else                                   { pf.encoding = PixelEncoding::Unknown; }
                    caps.pixelFormat.supported.append(pf);
                    if (curFmt && pf.nativeKey == QString::fromLocal8Bit(curFmt))
                        caps.pixelFormat.defaultFormat = pf;
                }
                g_free(fmts);
            }
            g_object_unref(cam);
        }
        g_clear_error(&err);

        DeviceEnumerator::instance()->registerDevice(
            DeviceType::VideoInput, Backend::Aravis,
            QString::fromLocal8Bit(deviceId),
            QString::fromLocal8Bit(model),
            caps);
    }
#endif

#ifdef HAVE_SPINNAKER
    // Only touch the SDK when it has been discovered and made loadable at runtime;
    // the imports are delay-loaded, so calling in without the DLLs present would crash.
    if (pinpoint::spinnaker::runtimeAvailable()) try {
        Spinnaker::SystemPtr system = Spinnaker::System::GetInstance();
        Spinnaker::CameraList camList = system->GetCameras();
        for (unsigned int i = 0; i < camList.GetSize(); ++i) {
            Spinnaker::CameraPtr cam = camList.GetByIndex(i);
            Spinnaker::GenApi::INodeMap &tlMap = cam->GetTLDeviceNodeMap();

            auto readTLStr = [&tlMap](const char *name) -> QString {
                Spinnaker::GenApi::CStringPtr n = tlMap.GetNode(name);
                if (Spinnaker::GenApi::IsAvailable(n) && Spinnaker::GenApi::IsReadable(n))
                    return QString::fromStdString(n->GetValue().c_str());
                return {};
            };
            QString id     = readTLStr("DeviceID");
            QString model  = readTLStr("DeviceModelName");
            QString serial = readTLStr("DeviceSerialNumber");
            QString vendor = readTLStr("DeviceVendorName");
            QString iftype = readTLStr("DeviceType");
            if (id.isEmpty())    id    = "Unknown";
            if (model.isEmpty()) model = "Spinnaker Camera";

            // Init gives full GenICam nodemap access without starting acquisition.
            CameraCapabilities caps;
            caps.driverVersion  = "Teledyne Spinnaker SDK";
            caps.serialNumber   = serial;
            caps.vendorName     = vendor;
            caps.modelName      = model;
            if (iftype.contains(QLatin1String("USB3"), Qt::CaseInsensitive))
                caps.connectionInterface = CameraCapabilities::Interface::USB3;
            else if (iftype.contains(QLatin1String("USB2"), Qt::CaseInsensitive))
                caps.connectionInterface = CameraCapabilities::Interface::USB2;
            else if (iftype.contains(QLatin1String("GigE"), Qt::CaseInsensitive))
                caps.connectionInterface = CameraCapabilities::Interface::GigE;
            try {
                cam->Init();
                Spinnaker::GenApi::INodeMap &nodeMap = cam->GetNodeMap();

                using namespace Spinnaker::GenApi;
                CEnumerationPtr ptrPixelFormat = nodeMap.GetNode("PixelFormat");
                if (IsAvailable(ptrPixelFormat) && IsReadable(ptrPixelFormat)) {
                    caps.pixelFormat.kind     = CapabilityKind::Discrete;
                    caps.pixelFormat.writable = IsWritable(ptrPixelFormat);
                    NodeList_t entries;
                    ptrPixelFormat->GetEntries(entries);
                    for (INode *entry : entries) {
                        CEnumEntryPtr e(entry);
                        if (!IsAvailable(e) || !IsReadable(e)) continue;
                        QString key = QString::fromStdString(e->GetSymbolic().c_str());
                        PixelFormat pf;
                        pf.nativeKey = key;
                        if      (key == "BayerRG8")                    { pf.encoding = PixelEncoding::BayerRG8;  pf.bitsPerPixel = 8; }
                        else if (key == "BayerGB8")                    { pf.encoding = PixelEncoding::BayerGB8;  pf.bitsPerPixel = 8; }
                        else if (key == "BayerGR8")                    { pf.encoding = PixelEncoding::BayerGR8;  pf.bitsPerPixel = 8; }
                        else if (key == "BayerBG8")                    { pf.encoding = PixelEncoding::BayerBG8;  pf.bitsPerPixel = 8; }
                        else if (key == "BayerRG16")                   { pf.encoding = PixelEncoding::BayerRG16; pf.bitsPerPixel = 16; }
                        else if (key == "BayerGB16")                   { pf.encoding = PixelEncoding::BayerGB16; pf.bitsPerPixel = 16; }
                        else if (key == "Mono8")                       { pf.encoding = PixelEncoding::Mono8;     pf.bitsPerPixel = 8; }
                        else if (key == "Mono10")                      { pf.encoding = PixelEncoding::Mono10;    pf.bitsPerPixel = 10; }
                        else if (key == "Mono12")                      { pf.encoding = PixelEncoding::Mono12;    pf.bitsPerPixel = 12; }
                        else if (key == "Mono16")                      { pf.encoding = PixelEncoding::Mono16;    pf.bitsPerPixel = 16; }
                        else if (key == "RGB8Packed" || key == "RGB8") { pf.encoding = PixelEncoding::RGB8;      pf.bitsPerPixel = 24; }
                        else if (key == "BGR8")                        { pf.encoding = PixelEncoding::BGR8;      pf.bitsPerPixel = 24; }
                        else                                           { pf.encoding = PixelEncoding::Unknown; }
                        bool isDefault = (e->GetValue() == ptrPixelFormat->GetIntValue());
                        caps.pixelFormat.supported.append(pf);
                        if (isDefault) caps.pixelFormat.defaultFormat = pf;
                    }
                }

                CIntegerPtr ptrW = nodeMap.GetNode("Width");
                CIntegerPtr ptrH = nodeMap.GetNode("Height");
                if (IsAvailable(ptrW) && IsReadable(ptrW) &&
                    IsAvailable(ptrH) && IsReadable(ptrH)) {
                    // Range max must be the TRUE sensor dims (WidthMax/
                    // HeightMax): Width's own max is WidthMax - OffsetX, so
                    // it under-reports while a crop ROI is still programmed
                    // in the camera (ROI nodes are retained across app runs
                    // while the camera stays powered). CameraInstance sizes
                    // ring slots and the expected crop from this range max —
                    // a stale value here compounds the crop every connect.
                    CIntegerPtr ptrWMax = nodeMap.GetNode("WidthMax");
                    CIntegerPtr ptrHMax = nodeMap.GetNode("HeightMax");
                    const int sensorW = (IsAvailable(ptrWMax) && IsReadable(ptrWMax))
                                            ? (int)ptrWMax->GetValue() : (int)ptrW->GetMax();
                    const int sensorH = (IsAvailable(ptrHMax) && IsReadable(ptrHMax))
                                            ? (int)ptrHMax->GetValue() : (int)ptrH->GetMax();
                    caps.resolution.kind     = CapabilityKind::Range;
                    caps.resolution.writable = IsWritable(ptrW);
                    caps.resolution.widthRange  = { (int)ptrW->GetMin(), sensorW, (int)ptrW->GetInc(), (int)ptrW->GetValue() };
                    caps.resolution.heightRange = { (int)ptrH->GetMin(), sensorH, (int)ptrH->GetInc(), (int)ptrH->GetValue() };
                    caps.resolution.defaultResolution = { (int)ptrW->GetValue(), (int)ptrH->GetValue() };
                }

                // --- Frame rate ---
                // AcquisitionFrameRateEnable defaults to false on FLIR cameras
                // (auto rate), which causes GetMax() to return the
                // exposure-limited rate rather than the hardware maximum.
                // Enable it briefly to read the true hardware max, then restore.
                CBooleanPtr ptrFpsEnable = nodeMap.GetNode("AcquisitionFrameRateEnable");
                bool restoredFpsEnable = false;
                if (IsAvailable(ptrFpsEnable) && IsWritable(ptrFpsEnable)
                        && !ptrFpsEnable->GetValue()) {
                    ptrFpsEnable->SetValue(true);
                    restoredFpsEnable = true;
                }
                CFloatPtr ptrFps = nodeMap.GetNode("AcquisitionFrameRate");
                if (IsAvailable(ptrFps) && IsReadable(ptrFps)) {
                    caps.frameRate.kind               = CapabilityKind::Range;
                    caps.frameRate.readable           = true;
                    caps.frameRate.writable           = IsWritable(ptrFps);
                    caps.frameRate.range.min          = ptrFps->GetMin();
                    caps.frameRate.range.max          = ptrFps->GetMax();
                    caps.frameRate.range.step         = 0;
                    caps.frameRate.range.defaultValue = ptrFps->GetValue();
                }
                if (restoredFpsEnable && IsAvailable(ptrFpsEnable) && IsWritable(ptrFpsEnable))
                    ptrFpsEnable->SetValue(false);

                cam->DeInit();
            } catch (...) {}

            DeviceEnumerator::instance()->registerDevice(
                DeviceType::VideoInput, Backend::Spinnaker, id, model, caps);
        }
        camList.Clear();
        system->ReleaseInstance();
    } catch (...) {}
#endif
}

VideoInputBase* VideoInputFactory::create(Backend backend, QObject *parent)
{
    // Enumerate only when the registry is empty (first use / tests). A full
    // re-enumeration briefly OPENS every Aravis device and Init()/DeInit()s
    // every Spinnaker camera — including ones live-streaming under another
    // CameraInstance in this process (the settings crop-preview creates
    // instances mid-session) — and blocks the calling thread on GenICam node
    // reads. CameraManager already enumerates at startup; rescans are
    // explicit user actions.
    bool haveVideoDevices = false;
    for (const auto &dev : DeviceEnumerator::instance()->devices())
        if (dev.type == DeviceType::VideoInput) { haveVideoDevices = true; break; }
    if (!haveVideoDevices)
        enumerateDevices();

    if (backend == Backend::Auto) {
#ifdef HAVE_SPINNAKER
        if (pinpoint::spinnaker::runtimeAvailable()) {
            for (const auto &dev : DeviceEnumerator::instance()->devices()) {
                if (dev.backend == Backend::Spinnaker) {
                    ppInfo() << "[VideoInputFactory] Spinnaker camera detected; selecting Spinnaker backend.";
                    return new VideoInputSpinnaker(parent);
                }
            }
        }
#endif

#ifdef HAVE_ARAVIS
        {
            bool hasAravis = false;
            for (const auto &dev : DeviceEnumerator::instance()->devices())
                if (dev.backend == Backend::Aravis) { hasAravis = true; break; }
            if (hasAravis) {
                ppInfo() << "[VideoInputFactory] Industrial camera(s) detected; selecting Aravis backend.";
                return new VideoInputAravis(parent);
            }
        }
#endif

#ifdef Q_OS_MACOS
        ppInfo() << "[VideoInputFactory] Selecting native Apple AVFoundation backend.";
        return new VideoInputApple(parent);
#endif

        ppInfo() << "[VideoInputFactory] Selecting Qt Multimedia backend.";
        return new VideoInput(parent);
    }

    switch (backend) {
        case Backend::QtMultimedia:
            return new VideoInput(parent);
#ifdef Q_OS_MACOS
        case Backend::AppleAVFoundation:
            return new VideoInputApple(parent);
#endif
#ifdef HAVE_ARAVIS
        case Backend::Aravis:
            return new VideoInputAravis(parent);
#endif
#ifdef HAVE_SPINNAKER
        case Backend::Spinnaker:
            if (pinpoint::spinnaker::runtimeAvailable())
                return new VideoInputSpinnaker(parent);
            ppWarn() << "[VideoInputFactory] Spinnaker backend requested but SDK not found; falling back to Qt Multimedia.";
            return new VideoInput(parent);
#endif
#ifdef HAVE_PPCP
        // ⚠ NO FALLBACK HERE, DELIBERATELY. Every other backend falls back to
        // Qt Multimedia when its SDK is missing, because they are all ways of
        // reaching a camera plugged into THIS machine. A PPCP camera is a
        // camera on another device; substituting a local one would hand the
        // caller a different camera under the same id.
        case Backend::Ppcp:
            return new VideoInputPpcp(parent);
#endif
        default:
            ppWarn() << "[VideoInputFactory] Requested backend not available on this platform; falling back to Qt Multimedia.";
            return new VideoInput(parent);
    }
}

VideoInputFactory::Backend VideoInputFactory::backendType(VideoInputBase *input)
{
    if (!input) return Backend::QtMultimedia;
#ifdef HAVE_PPCP
    if (dynamic_cast<VideoInputPpcp*>(input)) return Backend::Ppcp;
#endif
#ifdef HAVE_SPINNAKER
    if (dynamic_cast<VideoInputSpinnaker*>(input)) return Backend::Spinnaker;
#endif
#ifdef HAVE_ARAVIS
    if (dynamic_cast<VideoInputAravis*>(input)) return Backend::Aravis;
#endif
#ifdef Q_OS_MACOS
    if (dynamic_cast<VideoInputApple*>(input)) return Backend::AppleAVFoundation;
#endif
    return Backend::QtMultimedia;
}

QList<VideoInputFactory::Backend> VideoInputFactory::availableBackends()
{
    QList<Backend> list;
    list << Backend::QtMultimedia;
#ifdef HAVE_PPCP
    // Always available where the library is linked: whether a PPCP camera
    // EXISTS is a question about who is connected, not about this build.
    list << Backend::Ppcp;
#endif
#ifdef Q_OS_MACOS
    list << Backend::AppleAVFoundation;
#endif
#ifdef HAVE_ARAVIS
    list << Backend::Aravis;
#endif
#ifdef HAVE_SPINNAKER
    if (pinpoint::spinnaker::runtimeAvailable())
        list << Backend::Spinnaker;
#endif
    return list;
}

#ifdef HAVE_PPCP
int VideoInputFactory::registerPpcpPeer(const struct ppcp_peer_desc *peer)
{
    // One line, and it is a forward: VideoInputPpcp_inventory.cpp is the only
    // translation unit in src/Video that touches DeviceEnumerator on the PPCP
    // path, so that the capability mapping itself stays a pure function of a
    // declaration and testable with no registry in the link.
    return VideoInputPpcp::registerSources(peer);
}
#endif
