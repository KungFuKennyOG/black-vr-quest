# Signing key (debug)

`dbg.jks` is the debug keystore every BLACK VR APK so far has been signed with (store password `android`).
Android only installs an update over the app if it is signed with the same key. A new workspace has to use
this file, or the headset needs an uninstall first (game files and saves in /sdcard/BlackVR survive that).

    BLACKVR_KEYSTORE=$PWD/port/quest/keys/dbg.jks BLACKVR_KEYSTORE_PASS=android port/quest/build_apk.sh --with-game

The repo is private. Remove this file (and rotate to a new key) before the repository is ever made public.
