# ec-sss
bgfx example style project for screen space shadows, work in progress

# building
1) setup bgfx
2) add these files to new folder in 'examples', like examples\xx-sss
3) edit 'scripts\genie.lua' and 'examples\makefile' to add this new example to list of examples
4) run makefile in this folder to compile shaders

# notes
Implement screen space shadows as bgfx example. Goal is to explore various options and parameters. Includes toggle for "soft contact shadows", inspired by screen space ambient occlusion. These could fit better with the look of your scene if you already have some kind of soft shadows.

# references
