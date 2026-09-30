#pragma once

#ifdef __cplusplus

#include <string>

// HiveShock: localhost TCP command server for the HiveShock companion app.
// Protocol: one JSON object per line on 127.0.0.1:<Port> (default 43000).
// Supported on Windows, macOS and Linux; a no-op stub on Switch / Wii U.
class HiveShock {
  public:
    static HiveShock* Instance;

    void Enable();
    void Disable();
    bool IsEnabled() const;
    bool IsListening() const;
    bool HasClient() const;
    std::string GetListenEndpoint() const;
};

#endif // __cplusplus

#ifdef __cplusplus
extern "C" {
#endif

float HiveShock_GetOutgoingDamageMultiplier(void);

#ifdef __cplusplus
}
#endif
