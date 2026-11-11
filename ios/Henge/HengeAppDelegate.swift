import UIKit

/// The app's UIKit delegate, for what SwiftUI's App cannot express.
final class HengeAppDelegate: NSObject, UIApplicationDelegate {
    func application(_ application: UIApplication, didFinishLaunchingWithOptions launchOptions: [UIApplication.LaunchOptionsKey: Any]? = nil) -> Bool {
        ImageDownloads.shared.connect() // macOS downloads carry on from where they were
        NetworkLink.start() // the Mac's Ethernet cable follows the iPad's network
        return true
    }

    /// iOS opened Henge in the background because download pieces finished (ImageDownloads).
    func application(_ application: UIApplication, handleEventsForBackgroundURLSession identifier: String,
                     completionHandler: @escaping () -> Void) {
        ImageDownloads.shared.backgroundEventsDone = completionHandler
        ImageDownloads.shared.connect()
    }

    /// Before iOS 27 the external display scene connects through Info.plist's
    /// UIWindowSceneSessionRoleExternalDisplayNonInteractive entry (ExternalDisplaySceneDelegate).
    /// With the SwiftUI lifecycle UIKit asks here for the application role only, never for that
    /// one, with or without the plist entry; this records what it is asked so a device log shows it.
    func application(_ application: UIApplication, configurationForConnecting session: UISceneSession,
                     options: UIScene.ConnectionOptions) -> UISceneConfiguration {
        let config = UISceneConfiguration(name: nil, sessionRole: session.role)
        HengeLog.log("display", "scene configuration asked for role \(session.role.rawValue)")
        if session.role == .windowExternalDisplayNonInteractive, LegacyExternalDisplay.offeredToAppDelegate {
            config.delegateClass = ExternalDisplaySceneDelegate.self
        }
        return config
    }
}
