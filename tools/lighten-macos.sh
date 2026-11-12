#!/bin/sh
# Makes macOS lighter on an emulated Mac: turns off background work and animation that cost
# far more under Henge than on a real Mac. Run it in the Mac's Terminal. It asks for your
# password once (sudo). tools/lighten-macos-undo.sh puts everything back.
#
# Not done here, because macOS only lets you do them by hand in System Settings:
#   Accessibility > Display: Reduce motion, Reduce transparency (if the lines below say so)
#   General > Login Items: remove what you don't use
#   Siri, Apple Intelligence, iCloud: off, or never signed in
#   Wallpaper: a still picture; Screen Saver: never

echo "Spotlight indexing off (the heaviest background job on a new install)..."
sudo mdutil -a -i off

echo "Automatic update checks and downloads off..."
sudo softwareupdate --schedule off
sudo defaults write /Library/Preferences/com.apple.SoftwareUpdate AutomaticDownload -bool false

echo "Time Machine's automatic backups and Power Nap off..."
sudo tmutil disable
sudo pmset -a powernap 0

echo "Window, Dialog, Quick Look and WindowServer animation speedups..."
defaults write NSGlobalDomain NSAutomaticWindowAnimationsEnabled -bool false
defaults write NSGlobalDomain NSWindowResizeTime -float 0.001
defaults write NSGlobalDomain NSScrollAnimationEnabled -bool false
defaults write NSGlobalDomain QLPanelAnimationDuration -float 0
defaults write NSGlobalDomain NSWindowDocumentVersionsFileProviderSpringLoadingDelay -float 0
defaults write NSGlobalDomain NSToolbarTitleViewRolloverDelay -float 0
defaults write NSGlobalDomain NSDocumentSaveNewDocumentsToCloud -bool false
defaults write NSGlobalDomain NSNavPanelExpandedStateForSaveMode -bool true
defaults write NSGlobalDomain NSNavPanelExpandedStateForSaveMode2 -bool true
defaults write NSGlobalDomain PMPrintingExpandedStateForPrint -bool true
defaults write NSGlobalDomain PMPrintingExpandedStateForPrint2 -bool true
defaults write NSGlobalDomain CGDisableCursorLocationMagnification -bool true

echo "Dock, Spaces and Mission Control speedups and zero autohide delays..."
defaults write com.apple.dock launchanim -bool false
defaults write com.apple.dock expose-animation-duration -float 0.05
defaults write com.apple.dock autohide-delay -float 0
defaults write com.apple.dock autohide-time-modifier -float 0
defaults write com.apple.dock mineffect -string scale
defaults write com.apple.dock workspaces-swoosh-animation-off -bool true
defaults write com.apple.dock mru-spaces -bool false
defaults write com.apple.dock expose-group-apps -bool false

echo "Finder animations off..."
defaults write com.apple.finder DisableAllAnimations -bool true
defaults write com.apple.finder AnimateWindowZoom -bool false
defaults write com.apple.finder AnimateInfoPanes -bool false
defaults write com.apple.finder AnimateSnapToGrid -bool false

echo "Reduce motion and transparency..."
if defaults write com.apple.universalaccess reduceMotion -bool true 2>/dev/null &&
   defaults write com.apple.universalaccess reduceTransparency -bool true 2>/dev/null; then
    echo "  done (user preferences)"
else
    echo "  macOS refused user domain: turn both on in System Settings > Accessibility > Display"
fi
sudo defaults write /Library/Preferences/com.apple.universalaccess reduceMotion -bool true 2>/dev/null || true
sudo defaults write /Library/Preferences/com.apple.universalaccess reduceTransparency -bool true 2>/dev/null || true

echo "Photos and media background analysis off..."
launchctl disable "gui/$(id -u)/com.apple.photoanalysisd" 2>/dev/null
launchctl disable "gui/$(id -u)/com.apple.mediaanalysisd" 2>/dev/null
killall photoanalysisd mediaanalysisd 2>/dev/null

echo "Intelligence and suggestions background indexing off..."
launchctl disable "gui/$(id -u)/com.apple.intelligenceplatformd" 2>/dev/null
launchctl disable "gui/$(id -u)/com.apple.suggestd" 2>/dev/null
launchctl disable "gui/$(id -u)/com.apple.triald" 2>/dev/null
launchctl disable "gui/$(id -u)/com.apple.knowledgeconstructiond" 2>/dev/null
launchctl disable "gui/$(id -u)/com.apple.biometrickitd" 2>/dev/null
killall intelligenceplatformd suggestd triald 2>/dev/null

echo "Diagnostic and spin hang profiling off..."
sudo launchctl disable system/com.apple.spindump 2>/dev/null
sudo launchctl disable system/com.apple.tailspind 2>/dev/null
sudo launchctl disable system/com.apple.analyticsd 2>/dev/null
sudo launchctl disable system/com.apple.diagnosticd 2>/dev/null

killall Dock Finder 2>/dev/null
echo "Done. Log out and in again (or restart the Mac) for all of it to take effect."
