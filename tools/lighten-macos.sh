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

echo "Window, Dock and Finder animation off..."
defaults write NSGlobalDomain NSAutomaticWindowAnimationsEnabled -bool false
defaults write NSGlobalDomain NSWindowResizeTime -float 0.001
defaults write NSGlobalDomain NSScrollAnimationEnabled -bool false
defaults write com.apple.dock launchanim -bool false
defaults write com.apple.dock expose-animation-duration -float 0.1
defaults write com.apple.dock autohide-time-modifier -float 0
defaults write com.apple.dock mineffect -string scale
defaults write com.apple.finder DisableAllAnimations -bool true

echo "Reduce motion and transparency..."
if defaults write com.apple.universalaccess reduceMotion -bool true 2>/dev/null &&
   defaults write com.apple.universalaccess reduceTransparency -bool true 2>/dev/null; then
    echo "  done"
else
    echo "  macOS refused (Terminal has no Full Disk Access): turn both on yourself in"
    echo "  System Settings > Accessibility > Display"
fi

killall Dock Finder 2>/dev/null
echo "Done. Log out and in again (or restart the Mac) for all of it to take effect."
