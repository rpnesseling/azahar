// Copyright 2015 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include "common/assert.h"
#include "common/logging/log.h"
#include "common/string_util.h"
#include "core/core.h"
#include "core/hle/applets/swkbd.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/shared_memory.h"
#include "core/hle/result.h"
#include "core/hle/service/gsp/gsp.h"
#include "core/hle/service/gsp/gsp_gpu.h"
#include "core/hle/service/hid/hid.h"
#include "core/memory.h"
#include "video_core/utils.h"

namespace HLE::Applets {

namespace {

u32 GetDisplayBufferModePixelSize(Service::APT::DisplayBufferMode mode) {
    switch (mode) {
    case Service::APT::DisplayBufferMode::R8G8B8A8:
    case Service::APT::DisplayBufferMode::R8G8B8:
        return 3;
    case Service::APT::DisplayBufferMode::R5G6B5:
    case Service::APT::DisplayBufferMode::R5G5B5A1:
    case Service::APT::DisplayBufferMode::R4G4B4A4:
        return 2;
    case Service::APT::DisplayBufferMode::Unimportable:
        return 0;
    default:
        UNREACHABLE_MSG("Unknown display buffer mode {}", mode);
        return 0;
    }
}

struct PixelColor {
    std::array<u8, 3> bytes;
    u32 bytes_per_pixel;
};

PixelColor EncodeColor(Service::APT::DisplayBufferMode mode, u8 red, u8 green, u8 blue) {
    switch (mode) {
    case Service::APT::DisplayBufferMode::R8G8B8A8:
    case Service::APT::DisplayBufferMode::R8G8B8:
        return {{blue, green, red}, 3};
    case Service::APT::DisplayBufferMode::R5G6B5: {
        const u16 pixel = static_cast<u16>(((red >> 3) << 11) | ((green >> 2) << 5) | (blue >> 3));
        return {{{static_cast<u8>(pixel), static_cast<u8>(pixel >> 8), 0}}, 2};
    }
    case Service::APT::DisplayBufferMode::R5G5B5A1: {
        const u16 pixel =
            static_cast<u16>(((red >> 3) << 11) | ((green >> 3) << 6) | ((blue >> 3) << 1) | 1);
        return {{{static_cast<u8>(pixel), static_cast<u8>(pixel >> 8), 0}}, 2};
    }
    case Service::APT::DisplayBufferMode::R4G4B4A4: {
        const u16 pixel =
            static_cast<u16>(((red >> 4) << 12) | ((green >> 4) << 8) | ((blue >> 4) << 4) | 0xF);
        return {{{static_cast<u8>(pixel), static_cast<u8>(pixel >> 8), 0}}, 2};
    }
    default:
        return {{}, 0};
    }
}

void SetPixel(u8* framebuffer, u32 x, u32 y, const PixelColor& color) {
    const auto offset = VideoCore::GetMortonOffset(x, y, color.bytes_per_pixel) +
                        (y & ~7) * Service::GSP::FRAMEBUFFER_WIDTH_POW2 * color.bytes_per_pixel;
    std::memcpy(framebuffer + offset, color.bytes.data(), color.bytes_per_pixel);
}

void FillRect(u8* framebuffer, u32 left, u32 top, u32 width, u32 height, const PixelColor& color) {
    const u32 right = std::min(left + width, Service::GSP::FRAMEBUFFER_WIDTH);
    const u32 bottom = std::min(top + height, Service::GSP::BOTTOM_FRAMEBUFFER_HEIGHT);

    for (u32 y = top; y < bottom; ++y) {
        for (u32 x = left; x < right; ++x) {
            SetPixel(framebuffer, x, y, color);
        }
    }
}

} // namespace

Result SoftwareKeyboard::ReceiveParameterImpl(Service::APT::MessageParameter const& parameter) {
    switch (parameter.signal) {
    case Service::APT::SignalType::Request: {
        // The LibAppJustStarted message contains a buffer with the size of the framebuffer shared
        // memory.
        // Create the SharedMemory that will hold the framebuffer data
        ASSERT(sizeof(capture_info) == parameter.buffer.size());

        std::memcpy(&capture_info, parameter.buffer.data(), sizeof(capture_info));

        using Kernel::MemoryPermission;
        // Create a SharedMemory that directly points to this heap block.
        framebuffer_memory = system.Kernel().CreateSharedMemoryForApplet(
            0, capture_info.size, MemoryPermission::ReadWrite, MemoryPermission::ReadWrite,
            "SoftwareKeyboard Memory");

        // Send the response message with the newly created SharedMemory
        SendParameter({
            .sender_id = id,
            .destination_id = parent,
            .signal = Service::APT::SignalType::Response,
            .object = framebuffer_memory,
        });

        return ResultSuccess;
    }

    case Service::APT::SignalType::Message: {
        // Callback result
        ASSERT_MSG(parameter.buffer.size() == sizeof(config),
                   "The size of the parameter (SoftwareKeyboardConfig) is wrong");

        std::memcpy(&config, parameter.buffer.data(), parameter.buffer.size());

        switch (config.callback_result) {
        case SoftwareKeyboardCallbackResult::OK:
            // Finish execution
            Finalize();
            return ResultSuccess;

        case SoftwareKeyboardCallbackResult::Close:
            // Let the frontend display error and quit
            frontend_applet->ShowError(Common::UTF16BufferToUTF8(config.callback_msg));
            config.return_code = SoftwareKeyboardResult::BannedInput;
            config.text_offset = config.text_length = 0;
            Finalize();
            return ResultSuccess;

        case SoftwareKeyboardCallbackResult::Continue:
            // Let the frontend display error and get input again
            // The input will be sent for validation again on next Update().
            frontend_applet->ShowError(Common::UTF16BufferToUTF8(config.callback_msg));
            frontend_applet->Execute(ToFrontendConfig(config));
            return ResultSuccess;

        default:
            UNREACHABLE();
        }
    }

    default: {
        LOG_ERROR(Service_APT, "unsupported signal {}", parameter.signal);
        UNIMPLEMENTED();
        // TODO(Subv): Find the right error code
        return ResultUnknown;
    }
    }
}

Result SoftwareKeyboard::Start(Service::APT::MessageParameter const& parameter) {
    ASSERT_MSG(parameter.buffer.size() == sizeof(config),
               "The size of the parameter (SoftwareKeyboardConfig) is wrong");

    std::memcpy(&config, parameter.buffer.data(), parameter.buffer.size());
    text_memory = std::static_pointer_cast<Kernel::SharedMemory, Kernel::Object>(parameter.object);

    DrawScreenKeyboard();

    using namespace Frontend;
    frontend_applet = system.GetSoftwareKeyboard();
    ASSERT(frontend_applet);

    frontend_applet->Execute(ToFrontendConfig(config));

    return ResultSuccess;
}

void SoftwareKeyboard::Update() {
    if (!frontend_applet->DataReady())
        return;

    using namespace Frontend;
    const KeyboardData& data = frontend_applet->ReceiveData();
    std::u16string text = Common::UTF8ToUTF16(data.text);
    // Include a null terminator
    std::memcpy(text_memory->GetPointer(), text.c_str(), (text.length() + 1) * sizeof(char16_t));
    switch (config.num_buttons_m1) {
    case SoftwareKeyboardButtonConfig::SingleButton:
        config.return_code = SoftwareKeyboardResult::D0Click;
        break;
    case SoftwareKeyboardButtonConfig::DualButton:
        if (data.button == 0)
            config.return_code = SoftwareKeyboardResult::D1Click0;
        else
            config.return_code = SoftwareKeyboardResult::D1Click1;
        break;
    case SoftwareKeyboardButtonConfig::TripleButton:
        if (data.button == 0)
            config.return_code = SoftwareKeyboardResult::D2Click0;
        else if (data.button == 1)
            config.return_code = SoftwareKeyboardResult::D2Click1;
        else
            config.return_code = SoftwareKeyboardResult::D2Click2;
        break;
    case SoftwareKeyboardButtonConfig::NoButton:
        // TODO: find out what is actually returned
        config.return_code = SoftwareKeyboardResult::None;
        break;
    default:
        LOG_CRITICAL(Applet_SWKBD, "Unknown button config {}", config.num_buttons_m1);
        UNREACHABLE();
    }

    config.text_length = static_cast<u16>(text.size());
    config.text_offset = 0;

    if (config.filter_flags & HLE::Applets::SoftwareKeyboardFilter::Callback) {
        std::vector<u8> buffer(sizeof(SoftwareKeyboardConfig));
        std::memcpy(buffer.data(), &config, buffer.size());

        // Send the message to invoke callback
        SendParameter({
            .sender_id = id,
            .destination_id = parent,
            .signal = Service::APT::SignalType::Message,
            .buffer = buffer,
        });
    } else {
        Finalize();
    }
}

void SoftwareKeyboard::DrawScreenKeyboard() {
    if (!framebuffer_memory) {
        return;
    }

    const auto bytes_per_pixel = GetDisplayBufferModePixelSize(capture_info.bottom_screen_format);
    if (bytes_per_pixel == 0) {
        return;
    }

    const auto framebuffer_size =
        Service::GSP::FRAMEBUFFER_WIDTH_POW2 * Service::GSP::BOTTOM_FRAMEBUFFER_HEIGHT *
        bytes_per_pixel;
    const auto framebuffer_offset = static_cast<u32>(capture_info.bottom_screen_left_offset);
    if (framebuffer_offset + framebuffer_size > framebuffer_memory->GetSize()) {
        return;
    }

    auto* framebuffer = framebuffer_memory->GetPointer(framebuffer_offset);
    const auto background = EncodeColor(capture_info.bottom_screen_format, 0xE8, 0xE8, 0xE8);
    const auto input = EncodeColor(capture_info.bottom_screen_format, 0xFF, 0xFF, 0xFF);
    const auto key_face = EncodeColor(capture_info.bottom_screen_format, 0xF9, 0xF9, 0xF9);
    const auto border = EncodeColor(capture_info.bottom_screen_format, 0xC4, 0xC4, 0xC4);

    FillRect(framebuffer, 0, 0, Service::GSP::FRAMEBUFFER_WIDTH, Service::GSP::BOTTOM_FRAMEBUFFER_HEIGHT,
             background);
    FillRect(framebuffer, 8, 8, 224, 46, input);

    constexpr u32 key_width = 20;
    constexpr u32 key_height = 30;
    constexpr u32 key_gap = 4;
    constexpr u32 row_left = 8;
    constexpr std::array<u32, 4> row_tops = {72, 108, 144, 180};

    for (u32 row = 0; row < row_tops.size(); ++row) {
        const u32 key_count = row == 3 ? 8 : 10;
        const u32 left = row_left + row * 10;
        for (u32 key = 0; key < key_count; ++key) {
            FillRect(framebuffer, left + key * (key_width + key_gap), row_tops[row], key_width,
                     key_height, key_face);
            FillRect(framebuffer, left + key * (key_width + key_gap), row_tops[row], key_width, 1,
                     border);
            FillRect(framebuffer, left + key * (key_width + key_gap), row_tops[row], 1, key_height,
                     border);
        }
    }

    FillRect(framebuffer, 32, 222, 176, 28, key_face);
    FillRect(framebuffer, 32, 222, 176, 1, border);
    FillRect(framebuffer, 32, 222, 1, 28, border);
}

Result SoftwareKeyboard::Finalize() {
    std::vector<u8> buffer(sizeof(SoftwareKeyboardConfig));
    std::memcpy(buffer.data(), &config, buffer.size());
    CloseApplet(nullptr, buffer);
    text_memory = nullptr;
    return ResultSuccess;
}

Frontend::KeyboardConfig SoftwareKeyboard::ToFrontendConfig(
    const SoftwareKeyboardConfig& config) const {
    using namespace Frontend;
    KeyboardConfig frontend_config;
    frontend_config.button_config =
        static_cast<ButtonConfig>(static_cast<u32>(config.num_buttons_m1));
    frontend_config.accept_mode = static_cast<AcceptedInput>(static_cast<u32>(config.valid_input));
    frontend_config.multiline_mode = config.multiline;
    frontend_config.max_text_length = config.max_text_length;
    frontend_config.max_digits = config.max_digits;
    frontend_config.hint_text = Common::UTF16BufferToUTF8(config.hint_text);
    for (const auto& text : config.button_text) {
        frontend_config.button_text.push_back(Common::UTF16BufferToUTF8(text));
    }
    frontend_config.filters.prevent_digit =
        static_cast<bool>(config.filter_flags & SoftwareKeyboardFilter::Digits);
    frontend_config.filters.prevent_at =
        static_cast<bool>(config.filter_flags & SoftwareKeyboardFilter::At);
    frontend_config.filters.prevent_percent =
        static_cast<bool>(config.filter_flags & SoftwareKeyboardFilter::Percent);
    frontend_config.filters.prevent_backslash =
        static_cast<bool>(config.filter_flags & SoftwareKeyboardFilter::Backslash);
    frontend_config.filters.prevent_profanity =
        static_cast<bool>(config.filter_flags & SoftwareKeyboardFilter::Profanity);
    frontend_config.filters.enable_callback =
        static_cast<bool>(config.filter_flags & SoftwareKeyboardFilter::Callback);
    return frontend_config;
}
} // namespace HLE::Applets
