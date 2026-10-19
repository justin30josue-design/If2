# Input Buffer

Clicked a few frames too early? This mod moves your click to the exact
frame that survives.

When you click, the mod saves the level state and quickly simulates the
next moment with your click placed 0, 1, 2 ... up to your setting frames
later. It picks the earliest frame where you survive, rewinds, and plays
your click on that frame. If you were too early even for your setting, the
click happens normally and you die like vanilla.

- **Max Early Frames**: 2-8.
- **Frame Unit**: 240 (GD physics steps) or 60 (60 FPS frames).
- **Look-ahead**: how far ahead survival is checked (default 45 steps,
  about 0.2 s). Higher is safer but heavier.
- **Safe Mode** (on by default): runs in test mode so nothing saves as a
  real completion.

Ship, wave and swing clicks are simulated as held; other modes as taps.
