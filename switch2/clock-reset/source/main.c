// Clock reset: shows the CPU / GPU / memory (EMC) clocks and sets them back to stock.
// clkrst rates set by a homebrew title stay set after it exits (fastcars --switch2_clocks raised
// EMC to 3200 MHz in handheld and the CPU then ran at half speed); this puts them back.
//   A: handheld stock  (CPU 1100, GPU 561,  EMC 2133 MHz - read on this console before any change)
//   X: docked stock    (CPU 1100, GPU 1007, EMC 3200 MHz)
//   +: exit

#include <stdio.h>
#include <switch.h>

typedef struct {
    const char* name;
    PcvModuleId id;
    ClkrstSession session;
    bool open;
} Module;

static Module g_modules[3] = {
    {"CPU", PcvModuleId_CpuBus},
    {"GPU", PcvModuleId_GPU},
    {"EMC", PcvModuleId_EMC},
};

static void PrintClocks(void) {
    for (int i = 0; i < 3; ++i) {
        u32 hz = 0;
        if (g_modules[i].open) clkrstGetClockRate(&g_modules[i].session, &hz);
        printf("  %s: %4u MHz%s\n", g_modules[i].name, hz / 1000000,
               g_modules[i].open ? "" : " (session failed)");
    }
}

static void SetClocks(const u32 mhz[3]) {
    for (int i = 0; i < 3; ++i) {
        if (!g_modules[i].open) continue;
        Result rc = clkrstSetClockRate(&g_modules[i].session, mhz[i] * 1000000u);
        printf("  set %s %u MHz: rc 0x%X\n", g_modules[i].name, mhz[i], rc);
    }
}

int main(int argc, char** argv) {
    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    printf("Clock reset\n\n");
    Result rc = clkrstInitialize();
    if (R_FAILED(rc)) {
        printf("clkrstInitialize failed: 0x%X\n", rc);
    } else {
        for (int i = 0; i < 3; ++i) {
            g_modules[i].open = R_SUCCEEDED(clkrstOpenSession(&g_modules[i].session, g_modules[i].id, 3));
        }
    }
    printf("Current clocks:\n");
    PrintClocks();
    printf("\nA: handheld stock (1100 / 561 / 2133)\n");
    printf("X: docked stock   (1100 / 1007 / 3200)\n");
    printf("+: exit\n\n");
    consoleUpdate(NULL);

    static const u32 kHandheld[3] = {1100, 561, 2133};
    static const u32 kDocked[3] = {1100, 1007, 3200};
    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 down = padGetButtonsDown(&pad);
        if (down & HidNpadButton_Plus) break;
        if (down & (HidNpadButton_A | HidNpadButton_X)) {
            SetClocks((down & HidNpadButton_A) ? kHandheld : kDocked);
            printf("Now:\n");
            PrintClocks();
            printf("\n");
        }
        consoleUpdate(NULL);
    }

    for (int i = 0; i < 3; ++i) {
        if (g_modules[i].open) clkrstCloseSession(&g_modules[i].session);
    }
    clkrstExit();
    consoleExit(NULL);
    return 0;
}
