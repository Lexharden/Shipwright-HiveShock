#pragma once

#ifdef __cplusplus

#include <string>

// HiveShock: localhost TCP command server for the HiveShock companion app.
// Protocol: one JSON object per line on 127.0.0.1:<Port> (default 43000).
// Supported on Windows, macOS and Linux; a no-op stub on Switch / Wii U.
class HiveShock {
  public:
    static HiveShock* Instance;

    // Enemy counters for this session (only enemies: fairies and bombchus are not counted).
    struct Stats {
        int total = 0;     // received so far (waiting + spawned)
        int spawned = 0;   // have entered the game
        int defeated = 0;  // spawned and no longer alive
        int alive = 0;     // alive right now
        int waiting = 0;   // still in the queue
        int eliteBank = 0;     // elite points in use inside the elite bank
        int eliteBankMax = 0;  // size of the elite bank (0 = no bank)
        int eliteWaiting = 0;  // elite enemies still in the queue
    };
    Stats GetStats() const;
    // Starts counting from now: what is alive or waiting stays counted, the rest is forgotten.
    void ResetStats();

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
