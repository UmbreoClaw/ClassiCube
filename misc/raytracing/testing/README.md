# Headless ray tracing test harness

Runs the game under Xvfb with Mesa's software OpenGL (llvmpipe, OpenGL 4.5 with compute
shaders) and captures the framebuffer, so the ray tracer can be exercised without a GPU.
See doc/raytracing-dev-notes.md section 6 for how it was used.

Setup (Ubuntu):

    sudo apt-get install xvfb libgl1-mesa-dri libx11-dev libxi-dev libgl1-mesa-dev
    mkdir -p ~/rt-test/run/maps ~/rt-test/run/texpacks ~/rt-test/fb
    curl -L -o ~/rt-test/run/texpacks/default.zip https://classicube.net/static/default.zip
    cp ClassiCube ~/rt-test/run/
    cp misc/raytracing/testing/* ~/rt-test/
    gcc -w ~/rt-test/xsend.c -o ~/rt-test/xsend -lX11 -ldl
    Xvfb :99 -screen 0 1024x768x24 -fbdir ~/rt-test/fb &

Capture a view (mode Off/Shadows/GI/Full, output name, width, height, seconds to wait):

    python3 ~/rt-test/make_scene.py ~/rt-test/run/maps/scene.cw 34 44 315 32 24
    EXTRA='singleplayerphysics=false\n' ~/rt-test/capture.sh Full overview 800 600 60

Block light check (dark cobble room with a fire block, compare against the game's fancy lighting):

    python3 ~/rt-test/make_room.py ~/rt-test/run/maps/room.cw
    MAP=room EXTRA='gfx-lightingmode=Fancy\nsingleplayerphysics=false\n' ~/rt-test/capture.sh Off  room_off  640 480 25
    MAP=room EXTRA='gfx-lightingmode=Fancy\nsingleplayerphysics=false\n' ~/rt-test/capture.sh Full room_full 640 480 40

The window is centred on the Xvfb screen, so crop at (screenW - W)/2, (screenH - H)/2.
`sidebyside.py`, `crop.py` and `px.py` compare captures (no PIL required).
