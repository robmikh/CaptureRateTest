#include "pch.h"

namespace winrt
{
    using namespace Windows::Foundation;
    using namespace Windows::Foundation::Numerics;
    using namespace Windows::UI;
    using namespace Windows::Graphics::Capture;
    using namespace Windows::Graphics::DirectX;
    using namespace Windows::Graphics::DirectX::Direct3D11;
    using namespace Windows::System;
}

namespace util
{
    using namespace robmikh::common::uwp;
    using namespace robmikh::common::desktop;
    using namespace robmikh::common::wcli;
}

template<class... Ts> struct overloaded : Ts... { using Ts::operator()...; };
template<class... Ts> overloaded(Ts...) -> overloaded<Ts...>;

struct MonitorCaptureSubject
{
    uint32_t MonitorIndex;
};

struct WindowCaptureSubject
{
    std::wstring TitleQuery;
};

typedef std::variant<MonitorCaptureSubject, WindowCaptureSubject> CaptureSubject;

struct Options
{
    CaptureSubject Subject;
    uint32_t IntervalInMs;
};

struct MonitorCaptureItemSource
{
    HMONITOR Monitor;
};

struct WindowCaptureItemSource
{
    HWND Window;
};

typedef std::variant<MonitorCaptureItemSource, WindowCaptureItemSource> CaptureItemSource;

std::optional<Options> ParseOptions(int argc, wchar_t* argv[], bool& error);
std::optional<util::WindowInfo> GetWindowToCapture(std::vector<util::WindowInfo> const& windows);
std::optional<CaptureItemSource> CreateItemSourceFromSubject(CaptureSubject captureSubject);
std::optional<CaptureItemSource> CreateCaptureSourceFromMonitorIndex(uint32_t monitorIndex);
std::optional<CaptureItemSource> CreateCaptureSourceFromWindowTitleSearch(std::wstring const& titleQuery);
winrt::GraphicsCaptureItem CreateCaptureItemFromSource(CaptureItemSource itemSource);

int __stdcall wmain(int argc, wchar_t* argv[])
{
    // Initialize COM
    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    // Parse args
    bool error = false;
    auto options = ParseOptions(argc, argv, error);
    if (error || !options.has_value())
    {
        return error ? 1 : 0;
    }
    uint32_t interval = options->IntervalInMs;
    auto sourceOpt = CreateItemSourceFromSubject(options->Subject);
    if (!sourceOpt.has_value())
    {
        return 1;
    }
    auto source = sourceOpt.value();

    // Init D3D
    auto d3dDevice = util::CreateD3DDevice();
    auto dxgiDevice = d3dDevice.as<IDXGIDevice>();
    auto device = CreateDirect3DDevice(dxgiDevice.get());

    // Create our capture thread
    auto controller = winrt::DispatcherQueueController::CreateOnDedicatedThread();
    auto queue = controller.DispatcherQueue();

    // Setup capture and timer
    winrt::Direct3D11CaptureFrame frame{ nullptr };
    winrt::Direct3D11CaptureFramePool framePool{ nullptr };
    winrt::GraphicsCaptureSession session{ nullptr };
    winrt::DispatcherQueueTimer timer{ nullptr };
    wil::shared_event captureThreadEvent(wil::EventOptions::None);
    queue.TryEnqueue([captureThreadEvent, &framePool, &session, &frame, &timer, device, source, interval, queue]()
        {
            auto item = CreateCaptureItemFromSource(source);
            framePool = winrt::Direct3D11CaptureFramePool::Create(
                device,
                winrt::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                1,
                item.Size());
            session = framePool.CreateCaptureSession(item);
            framePool.FrameArrived([&frame](auto&& sender, auto&&)
                {
                    frame = sender.TryGetNextFrame();
                });

            timer = queue.CreateTimer();
            timer.Interval(std::chrono::milliseconds(interval));
            timer.IsRepeating(true);

            timer.Tick([&frame](auto&&, auto&&)
                {
                    if (frame != nullptr)
                    {
                        frame.Close();
                    }
                });
            timer.Start();

            session.StartCapture();
            captureThreadEvent.SetEvent();
        });
    captureThreadEvent.wait();

    // Wait
    wprintf(L"Press ENTER to exit...\n");
    std::wstring tempString;
    std::getline(std::wcin, tempString);

    captureThreadEvent.ResetEvent();
    queue.TryEnqueue([timer, framePool, session, captureThreadEvent]()
        {
            timer.Stop();
            framePool.Close();
            session.Close();
            captureThreadEvent.SetEvent();
        });
    captureThreadEvent.wait();
    wprintf(L"Shutting down capture thread...\n");
    controller.ShutdownQueueAsync().get();

    return 0;
}

winrt::GraphicsCaptureItem CreateCaptureItemFromSource(CaptureItemSource itemSource)
{
    return std::visit(overloaded
        {
            [=](MonitorCaptureItemSource const& source) -> winrt::GraphicsCaptureItem { return util::CreateCaptureItemForMonitor(source.Monitor); },
            [=](WindowCaptureItemSource const& source) -> winrt::GraphicsCaptureItem { return util::CreateCaptureItemForWindow(source.Window); },
        }, itemSource);
}

std::optional<CaptureItemSource> CreateItemSourceFromSubject(CaptureSubject captureSubject)
{
    return std::visit(overloaded
        {
            [=](MonitorCaptureSubject const& subject) -> std::optional<CaptureItemSource> { return CreateCaptureSourceFromMonitorIndex(subject.MonitorIndex); },
            [=](WindowCaptureSubject const& subject) -> std::optional<CaptureItemSource> { return CreateCaptureSourceFromWindowTitleSearch(subject.TitleQuery); },
        }, captureSubject);
}

std::optional<CaptureItemSource> CreateCaptureSourceFromMonitorIndex(uint32_t monitorIndex)
{
    std::vector<HMONITOR> monitors;
    winrt::check_bool(EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR hmon, HDC, LPRECT, LPARAM lparam)
        {
            auto& monitors = *reinterpret_cast<std::vector<HMONITOR>*>(lparam);
            monitors.push_back(hmon);

            return TRUE;
        }, reinterpret_cast<LPARAM>(&monitors)));

    if (monitorIndex < monitors.size())
    {
        return std::optional(CaptureItemSource(MonitorCaptureItemSource{ monitors[monitorIndex] }));
    }
    else
    {
        wprintf(L"Monitor index is out of bounds!\n");
        return std::nullopt;
    }
}

std::optional<CaptureItemSource> CreateCaptureSourceFromWindowTitleSearch(std::wstring const& titleQuery)
{
    auto windows = util::FindTopLevelWindowsByTitle(titleQuery);
    if (windows.size() == 0)
    {
        wprintf(L"No windows found!\n");
        return std::nullopt;
    }

    auto foundWindow = GetWindowToCapture(windows);
    if (!foundWindow.has_value())
    {
        return std::nullopt;
    }
    wprintf(L"Using window \"%s\"\n", foundWindow->Title.c_str());
    return std::optional(CaptureItemSource(WindowCaptureItemSource{ foundWindow->WindowHandle }));
}

std::optional<util::WindowInfo> GetWindowToCapture(std::vector<util::WindowInfo> const& windows)
{
    if (windows.empty())
    {
        throw winrt::hresult_invalid_argument();
    }

    auto numWindowsFound = windows.size();
    auto foundWindow = windows[0];
    if (numWindowsFound > 1)
    {
        wprintf(L"Found %I64u windows that match:\n", numWindowsFound);
        wprintf(L"    Num    PID       Window Title\n");
        auto count = 0;
        for (auto const& window : windows)
        {
            DWORD pid = 0;
            auto ignored = GetWindowThreadProcessId(window.WindowHandle, &pid);
            wprintf(L"    %3i    %06u    %s\n", count, pid, window.Title.c_str());
            count++;
        }

        do
        {
            wprintf(L"Please make a selection (q to quit): ");
            std::wstring selectionString;
            std::getline(std::wcin, selectionString);
            if (selectionString.rfind(L"q", 0) == 0 ||
                selectionString.rfind(L"Q", 0) == 0)
            {
                return std::nullopt;
            }
            auto selection = std::stoi(selectionString);
            if (selection >= 0 && selection < windows.size())
            {
                foundWindow = windows[selection];
                break;
            }
            else
            {
                wprintf(L"Invalid input, '%s'!\n", selectionString.c_str());
            }
        } while (true);
    }

    return std::optional(foundWindow);
}

std::optional<uint32_t> ParseNumberString(std::wstring const& numberString)
{
    try
    {
        uint32_t width = std::stoi(numberString);
        return std::optional(width);
    }
    catch (...)
    {
    }
    return std::nullopt;
}

std::optional<Options> ParseOptions(int argc, wchar_t* argv[], bool& error)
{
    error = true;
    std::vector<std::wstring> args(argv + 1, argv + argc);
    if (robmikh::common::wcli::impl::GetFlag(args, L"-help") || robmikh::common::wcli::impl::GetFlag(args, L"/?"))
    {
        wprintf(L"CaptureRateTest.exe\n");
        wprintf(L"A program to test the performance of Windows.Graphics.Capture while starving the DWM of buffers.\n");
        wprintf(L"\n");
        wprintf(L"Options:\n");
        wprintf(L"  -interval [value] (optional) Specify the capture interval in ms. Default is 1000.\n");
        wprintf(L"  -monitor  [value]            Specify the monitor to capture via index. Default is 0.\n");
        wprintf(L"                                 Conflicts with 'window'.\n");
        wprintf(L"  -window   [value]            Specify the window to capture via title substring search.\n");
        wprintf(L"                                 Conflicts with 'monitor'.\n");
        wprintf(L"\n");
        error = false;
        return std::nullopt;
    }
    auto intervalString = robmikh::common::wcli::impl::GetFlagValue(args, L"-interval", L"-i");
    auto monitorString = robmikh::common::wcli::impl::GetFlagValue(args, L"-monitor", L"-m");
    auto windowString = robmikh::common::wcli::impl::GetFlagValue(args, L"-window", L"-w");
    
    uint32_t interval = 1000;
    if (!intervalString.empty())
    {
        if (auto parsedInterval = ParseNumberString(intervalString))
        {
            interval = parsedInterval.value();
        }
        else
        {
            wprintf(L"Invalid interval specified!\n");
            return std::nullopt;
        }
    }

    if (!monitorString.empty() && !windowString.empty())
    {
        wprintf(L"Cannot use options 'monitor' and 'window' at the same time!\n");
        return std::nullopt;
    }

    uint32_t monitorIndex = 0;
    if (!monitorString.empty())
    {
        if (auto parsedIndex = ParseNumberString(intervalString))
        {
            interval = parsedIndex.value();
        }
        else
        {
            wprintf(L"Invalid monitor index specified!\n");
            return std::nullopt;
        }
    }

    CaptureSubject subject;
    if (!windowString.empty())
    {
        subject = CaptureSubject(WindowCaptureSubject{ windowString });
    }
    else
    {
        subject = CaptureSubject(MonitorCaptureSubject{ monitorIndex });
    }

    error = false;
    return std::optional(Options{ subject, interval });
}