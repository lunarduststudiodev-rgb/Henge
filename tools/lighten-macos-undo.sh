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
defaults delete com.apple.dock launchanim 2>/dev/null
defaults delete com.apple.dock expose-animation-duration 2>/dev/null
defaults delete com.apple.dock autohide-time-modifier 2>/dev/null
defaults delete com.apple.dock mineffect 2>/dev/null
defaults delete com.apple.finder DisableAllAnimations 2>/dev/null
defaults delete com.apple.universalaccess reduceMotion 2>/dev/null
defaults delete com.apple.universalaccess reduceTransparency 2>/dev/null

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
