#!/bin/sh
# Puts back what tools/lighten-macos.sh changed. Run it in the Mac's Terminal (asks for your password once).

sudo mdutil -a -i on
sudo softwareupdate --schedule on
sudo defaults delete /Library/Preferences/com.apple.SoftwareUpdate AutomaticDownload 2>/dev/null
sudo tmutil enable
sudo pmset -a powernap 1

defaults delete NSGlobalDomain NSAutomaticWindowAnimationsEnabled 2>/dev/null
defaults delete NSGlobalDomain NSWindowResizeTime 2>/dev/null
defaults delete NSGlobalDomain NSScrollAnimationEnabled 2>/dev/null
defaults delete NSGlobalDomain QLPanelAnimationDuration 2>/dev/null
defaults delete NSGlobalDomain NSWindowDocumentVersionsFileProviderSpringLoadingDelay 2>/dev/null
defaults delete NSGlobalDomain NSToolbarTitleViewRolloverDelay 2>/dev/null
defaults delete NSGlobalDomain NSDocumentSaveNewDocumentsToCloud 2>/dev/null
defaults delete NSGlobalDomain NSNavPanelExpandedStateForSaveMode 2>/dev/null
defaults delete NSGlobalDomain NSNavPanelExpandedStateForSaveMode2 2>/dev/null
defaults delete NSGlobalDomain PMPrintingExpandedStateForPrint 2>/dev/null
defaults delete NSGlobalDomain PMPrintingExpandedStateForPrint2 2>/dev/null
defaults delete NSGlobalDomain CGDisableCursorLocationMagnification 2>/dev/null
defaults delete NSGlobalDomain KeyRepeat 2>/dev/null
defaults delete NSGlobalDomain InitialKeyRepeat 2>/dev/null
defaults delete NSGlobalDomain ApplePressAndHoldEnabled 2>/dev/null
defaults delete NSGlobalDomain NSAutomaticSpellingCorrectionEnabled 2>/dev/null
defaults delete NSGlobalDomain NSAutomaticCapitalizationEnabled 2>/dev/null
defaults delete NSGlobalDomain NSAutomaticPeriodSubstitutionEnabled 2>/dev/null
defaults delete NSGlobalDomain NSAutomaticQuoteSubstitutionEnabled 2>/dev/null
defaults delete NSGlobalDomain NSAutomaticDashSubstitutionEnabled 2>/dev/null
defaults delete NSGlobalDomain AppleEnableSwipeNavigateWithScrolls 2>/dev/null
defaults delete NSGlobalDomain AppleMiniaturizeOnDoubleClick 2>/dev/null

defaults delete com.apple.dock launchanim 2>/dev/null
defaults delete com.apple.dock expose-animation-duration 2>/dev/null
defaults delete com.apple.dock autohide-delay 2>/dev/null
defaults delete com.apple.dock autohide-time-modifier 2>/dev/null
defaults delete com.apple.dock mineffect 2>/dev/null
defaults delete com.apple.dock workspaces-swoosh-animation-off 2>/dev/null
defaults delete com.apple.dock workspaces-auto-swoosh 2>/dev/null
defaults delete com.apple.dock appswitcher-all-displays 2>/dev/null
defaults delete com.apple.dock mru-spaces 2>/dev/null
defaults delete com.apple.dock expose-group-apps 2>/dev/null

defaults delete com.apple.finder DisableAllAnimations 2>/dev/null
defaults delete com.apple.finder AnimateWindowZoom 2>/dev/null
defaults delete com.apple.finder AnimateInfoPanes 2>/dev/null
defaults delete com.apple.finder AnimateSnapToGrid 2>/dev/null

defaults delete com.apple.universalaccess reduceMotion 2>/dev/null
defaults delete com.apple.universalaccess reduceTransparency 2>/dev/null
sudo defaults delete /Library/Preferences/com.apple.universalaccess reduceMotion 2>/dev/null
sudo defaults delete /Library/Preferences/com.apple.universalaccess reduceTransparency 2>/dev/null

launchctl enable "gui/$(id -u)/com.apple.photoanalysisd" 2>/dev/null
launchctl enable "gui/$(id -u)/com.apple.mediaanalysisd" 2>/dev/null
launchctl enable "gui/$(id -u)/com.apple.intelligenceplatformd" 2>/dev/null
launchctl enable "gui/$(id -u)/com.apple.suggestd" 2>/dev/null
launchctl enable "gui/$(id -u)/com.apple.triald" 2>/dev/null
launchctl enable "gui/$(id -u)/com.apple.knowledgeconstructiond" 2>/dev/null
launchctl enable "gui/$(id -u)/com.apple.biometrickitd" 2>/dev/null

sudo launchctl enable system/com.apple.spindump 2>/dev/null
sudo launchctl enable system/com.apple.tailspind 2>/dev/null
sudo launchctl enable system/com.apple.analyticsd 2>/dev/null
sudo launchctl enable system/com.apple.diagnosticd 2>/dev/null

killall Dock Finder 2>/dev/null
echo "Done. Log out and in again (or restart the Mac) for all of it to take effect."
