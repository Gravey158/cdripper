// Sparkle-Anbindung als Objective-C++-Bridge, damit cdripper (C++) das
// Update-Framework anstoßen kann ohne dass das gesamte Projekt auf
// Objective-C++ umgeschrieben wird.
//
// Wird NUR auf APPLE kompiliert (siehe CMakeLists). Ruft Sparkle's
// SPUStandardUpdaterController — der liest SUFeedURL aus dem Info.plist,
// vergleicht gegen den AppCast unter https://flatpak.x2-pandora.de/cdripper/
// appcast.xml und prompted den User wenn eine neue Version verfügbar ist.
//
// Auto-Check ist via SUEnableAutomaticChecks + SUScheduledCheckInterval im
// Info.plist konfiguriert (24h). Manuelle Trigger via
// cdripper_sparkle_check_for_updates() — könnte später aus dem Help-Menü
// der GUI aufgerufen werden.

#import <Foundation/Foundation.h>
#import <Sparkle/Sparkle.h>

static SPUStandardUpdaterController* g_updater = nil;

extern "C" void cdripper_sparkle_init(void) {
    if (g_updater) return;
    // initWithStartingUpdater:YES startet den Background-Check direkt.
    g_updater = [[SPUStandardUpdaterController alloc]
                 initWithStartingUpdater:YES
                          updaterDelegate:nil
                       userDriverDelegate:nil];
    // ARC retain auf static — sonst räumt das Objekt sofort wieder ab.
    (void)g_updater;
}

extern "C" void cdripper_sparkle_check_for_updates(void) {
    if (g_updater) {
        [g_updater checkForUpdates:nil];
    }
}
