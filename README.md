# ec-sss
bgfx example style project for screen space shadows

# building
1) setup bgfx
2) add these files to new folder in 'examples', like examples\xx-sss
3) edit 'scripts\genie.lua' and 'examples\makefile' to add this new example to list of examples
4) run makefile in this folder to compile shaders

# notes
Implement screen space shadows as bgfx example. Goal is to explore various options and parameters.

# radius
Use radius/shadow distance defined in screen space pixels or world units.

In world uints, the screen distance will shrink as objects get farther away. This can provide more natural looking shadows and fade out the effect at a distance, leaving screen space shadows as an added detail effect near the camera.

Screen space units mean that objects will cast the same length of shadow regardless of how far they are away from the camera. Pull back the camera and objects' shadows will appear to grow. On the other hand, this can be desired because it will allow objects at the horizon like hills and trees to cast a shadow. Depending on your scene, such far objects may be outside of the area affected by regular shadow maps. Even with multiple cascades, you may not be able to afford shadow maps across the entire scene.

This sample does not put effort into avoiding the initial pixel or avoiding resampling the same value if the step size is relatively smaller than the sampled distance in screen space. May want to set a minimum distance so each sample covers a unique value or take care to select a neighboring pixel for the first sample.

# soft contact shadows
If hard screen space shadows are added to a scene that already has soft shadows via shadow maps, the hard edge can look out of place. Additionally, it is common for screen space shadows to not quite line up with other shadows. This is because the depth buffer does not specify thickness, leaving some pixels incorrectly occluded. For example, you would not want some thin feature like a pipe to cast a shadow as if you were seeing the side of a metal wall.

These soft contact shadows are an attempt to minimize the problems described above. By adding a smoother falloff, they may blend into the scene better. Inspired by screen space ambient occlusion, this sample takes into account distance from shadowed pixel to its occluders.

hard - If there's any occluder found, mark the source pixel as shadowed.

soft - Modulate shadow by distance to the first occluder. Assuming a nearby pixel is closer and more likely to represent an accurate shadow, it is darker. If the first pixel to be an occluder is far away, it should likely cast a softer shadow.

very soft - In addition to the same modulation used by soft mode, also reduce the occlusion contribution from pixels that are farther away. This sample compares the depth difference to the shadow radius, a 1D distance, instead of comparing the actually distance in 3D space.

# references
