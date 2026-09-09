#ifndef CC_RAYTRACER_H
#define CC_RAYTRACER_H
#include "Core.h"
CC_BEGIN_HEADER

/*
Renders the world using GPU ray tracing (a voxel path tracer running in OpenGL compute shaders)
  instead of the rasterised chunk meshes. Provides ray traced shadows, one bounce global
  illumination, and reflections on water. Requires OpenGL 4.3 or later.
Copyright 2014-2025 ClassiCube | Licensed under BSD-3
*/
struct IGameComponent;
extern struct IGameComponent RayTracer_Component;

enum RayTracerMode {
	RT_MODE_OFF,     /* Rasterised rendering (default) */
	RT_MODE_SHADOWS, /* Ray traced sun shadows only */
	RT_MODE_GI,      /* Shadows and one bounce global illumination */
	RT_MODE_FULL,    /* Shadows, global illumination, soft shadows and water reflections */
	RT_MODE_COUNT
};
extern const char* const RayTracerMode_Names[RT_MODE_COUNT];

/* Currently selected mode (see RayTracerMode) */
extern int RayTracer_Mode;
/* Whether ray tracing is currently being used to render the world */
cc_bool RayTracer_Active(void);
/* Changes the ray tracing mode, and saves it to the options */
void RayTracer_SetMode(int mode);

/* Renders the world (normal and translucent blocks) using ray tracing */
void RayTracer_Render(float delta);
/* Notifies the ray tracer that a block in the world has changed */
void RayTracer_OnBlockChanged(int x, int y, int z, BlockID block);

CC_END_HEADER
#endif
