# The SDL3 release every build uses. The Android app's Java side
# (org.libsdl.app, fetched by platforms/android/FetchSdlJava.cmake) must come
# from the same release: SDL refuses to start when they differ.
set(PX_SDL_VERSION 3.4.18)
set(PX_SDL_URL "https://github.com/libsdl-org/SDL/releases/download/release-${PX_SDL_VERSION}/SDL3-${PX_SDL_VERSION}.tar.gz")
set(PX_SDL_SHA256 9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3)
