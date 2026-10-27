#pragma once

#include <cstddef>

namespace fastboot {

// Convert a Windows USB device-interface path such as
//   \\?\usb#vid_18d1&pid_4ee0#6&135b54c7&0&3#{a5dcbf10-...}
// into a compact, stable fastboot device path:
//   usb:1D00:0-1.3
//
// The first component is the PCI location of the USB host controller, the
// second is the Windows root-hub index, and the rest is the hub-port chain.
// Returns false when Windows does not expose a location path.
bool UsbPathFromInterfaceName(const wchar_t* interface_name, char* out, size_t out_size);

// Test whether a path (usb:... or a serial number) matches the last hub port
// and/or the final port index.  -1 means "don't care".  Missing numbers return
// false when a filter is requested.
bool UsbPathMatches(const char* path, int hub, int index);

}  // namespace fastboot
