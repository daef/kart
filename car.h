#ifndef CAR_H
#define CAR_H

#include <SDL2/SDL.h>

#include "defines.h"

typedef struct {
	vec2 direction;

	vec2 force;
	vec2 velocity;
	vec2 pos;

	int width, height;
	int wheel_x[4];
	int wheel_y[4];

	SDL_Texture *texture;
} Car;

void apply_force(Car *car, vec2 force);
void move_car(Car *car, SDL_Surface *map);

#endif

/* vim: set ts=8 sw=8 tw=0 noexpandtab cindent softtabstop=8 :*/