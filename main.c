#include <SDL2/SDL.h>
#include <time.h>
#include <stdio.h>
#include <string.h>

#include "box.h"
#include "car.h"
#include "map.h"
#include "net.h"
#include "powerup.h"
#include "renderer.h"
#include "sound.h"
#include "vector.h"
#include "shell.h"
#include "debug.h"
#include "libs/cJSON/cJSON.h"

static int sockfd = -1;
#define MAX_JSON_SIZE 64000
struct client {
	int idx;
	int fd;
	Car *car;
	SDL_mutex *cmd_lock;
	unsigned cmd;
	SDL_Thread *thr;
};
char *json_state;
SDL_mutex *json_state_lock;
static struct client clients[NUM_CLIENTS];
SDL_atomic_t net_listen;

typedef enum {
	MENU_SERVER,
	MENU_CLIENT,
	MENU_LOCAL,
	MENU_QUIT
} MenuChoice;

void do_render(SDL_Renderer *ren)
{
	render_background();
	map_render(ren);
	cars_move();
	boxes_render(ren);
	shells_move();
	shells_render(ren);
	cars_render(ren);

	static Uint32 last_time = 0;
	const Uint32 delta = SDL_GetTicks() - last_time;
	last_time = SDL_GetTicks();
	if (last_time && delta) {
		char buf[64];
		sprintf(buf, "%d fps", 1000 / delta);
		render_string(buf, SCREEN_WIDTH - 100, 5, 11);
	}
}

static int server_recv_loop(void *data)
{
	struct client *me = (struct client *)data;
	char buf[64];
	ssize_t n = 0;
	while (SDL_AtomicGet(&net_listen))
	{
		n = net_recv(me->fd, buf, 64);
		if (n > 0)
		{
			unsigned cmd;
			if (sscanf(buf, "%u", &cmd) == 1)
			{
				printf("T%d: Received: %d\n", me->idx, cmd);
				if (SDL_LockMutex(me->cmd_lock) == 0)
				{
					me->cmd = cmd;
					SDL_UnlockMutex(me->cmd_lock);
				}
				if (SDL_LockMutex(json_state_lock) == 0)
				{
					net_send(me->fd, json_state);
					SDL_UnlockMutex(json_state_lock);
				}
			}
		}
		else if (n == 0)
		{
			printf("T%d: Client closed connection\n", me->idx);
			break;
		}
		else
			break;
		SDL_Delay(10); // just to quiet things down a bit
	}
	return 0;
}

static int accpt_conn(void *data)
{
	int fd;
	SDL_atomic_t *got_client = (SDL_atomic_t*)data;
	fd = net_accept(sockfd);
	SDL_AtomicSet(got_client, 1);
	return fd;
}

char *json_to_text(cJSON *object)
{
	char *text = cJSON_Print(object);
	size_t total_length = strlen(text);
	cJSON_Minify(text);

	if (total_length - strlen(text) < 1) { // less than two extra bytes available
		json_state = realloc(text, total_length + 1);
	}
	size_t end = strlen(text);
	text[end] = '\n';
	text[end+1] = 0;
	return text;
}

int run_server(SDL_Renderer *ren)
{
	sockfd = net_start_server(NET_PORT);
	json_state_lock = SDL_CreateMutex();
	if (json_state_lock == NULL)
	{
		printf("Failed to create mutex\n");
		return 1;
	}

	cJSON *map_object = map_serialize();

	json_state = malloc(1);
	*json_state = 0;

	SDL_AtomicSet(&net_listen, 1);
	printf("Waiting for clients...\n");
	/* Set up each client */
	for (int i = 0; i < NUM_CLIENTS; i++)
	{
		SDL_Rect wfc_bg_target, car_target;
		SDL_Texture *wfc_bg_tex = ren_load_image("waitforclients.bmp");
		SDL_Texture *car_tex = ren_load_image_with_dims("car0.bmp", &car_target.w, &car_target.h);
		SDL_Event event;
		wfc_bg_target.x = 0;
		wfc_bg_target.y = 0;
		wfc_bg_target.w = SCREEN_WIDTH;
		wfc_bg_target.h = SCREEN_HEIGHT;
		car_target.x = SCREEN_WIDTH/2 - car_target.w/2;
		car_target.y = 280;
		float car_angle = 0;
		SDL_atomic_t got_client;
		SDL_AtomicSet(&got_client, 0);
		int clientfd;
		SDL_Thread *thr = SDL_CreateThread(accpt_conn, "Accpt conn thread", (void*)&got_client);
		if (thr == NULL)
		{
			printf("Failed to create accpt conn thread\n");
			return 1;
		}
		while (!SDL_AtomicGet(&got_client))
		{
			while (SDL_PollEvent(&event))
			{
				//If user closes the window
				if (event.type == SDL_QUIT) {
					return 0;
				}
				//If user presses any key
				if (event.type == SDL_KEYDOWN) {
					switch (event.key.keysym.sym) {
					case SDLK_ESCAPE:
						return 0;
					}
				}
			}
			SDL_RenderCopy(ren, wfc_bg_tex, 0, &wfc_bg_target);
			SDL_RenderCopyEx(ren, car_tex, 0, &car_target, car_angle, 0, 0);
			car_angle += 2*(i+1);
			if (car_angle >= 360) car_angle -= 360;
			for (int j = 0; j < NUM_CLIENTS; j++)
			{
				if (i > j)
					SDL_SetRenderDrawColor(ren, 0x00, 0xff, 0x00, 0xff);
				else
					SDL_SetRenderDrawColor(ren, 0xff, 0x00, 0x00, 0xff);
				SDL_Rect client_rect;
				client_rect.x = SCREEN_WIDTH/2 - ((NUM_CLIENTS-1)*10 + 5) + j * 20;
				client_rect.y = 378;
				client_rect.h = 10;
				client_rect.w = 10;
				SDL_RenderFillRect(ren, &client_rect);
			}
			SDL_RenderPresent(ren);
		}
		SDL_WaitThread(thr, &clientfd);
		clients[i].fd = clientfd;
		if (clients[i].fd < 0)
		{
			printf("Accept failed\n");
			return 1;
		}
		// Send initial state
		cJSON *initial_object = cJSON_CreateObject();
		cJSON_AddNumberToObject(initial_object, "id", i);
		cJSON_AddNumberToObject(initial_object, "num_cars", NUM_CLIENTS);
		cJSON_AddItemToObject(initial_object, "map", cJSON_Duplicate(map_object, 1));
		char *initial_json = json_to_text(initial_object);
		net_send(clients[i].fd, initial_json);
		cJSON_Delete(initial_object);
		free(initial_json);

		clients[i].car = car_add();

		clients[i].cmd_lock = SDL_CreateMutex();
		if (clients[i].cmd_lock == NULL)
		{
			printf("Failed to create mutex\n");
			return 1;
		}

		clients[i].thr = SDL_CreateThread(server_recv_loop, "Recv Client", &clients[i]);
		if (clients[i].thr == NULL)
		{
			printf("Failed to create recv thread\n");
			return 1;
		}
		printf("Num clients: %d\n", i+1);
	}
	printf("All clients connected\n");
	cJSON_Delete(map_object);
	SDL_SetRenderDrawColor(ren, 0x0, 0x0, 0x0, 0xff);
	SDL_RenderClear(ren);

	int quit = 0;
	SDL_Event event;
	Uint32 time0 = SDL_GetTicks();

	while (!quit) {
		while (SDL_PollEvent(&event))
		{
			//If user closes the window
			if (event.type == SDL_QUIT) {
				quit = 1;
			}
			//If user presses any key
			if (event.type == SDL_KEYDOWN) {
				switch (event.key.keysym.sym) {
				case SDLK_ESCAPE:
					quit = 1;
					break;
				}
			}
		}

		for (int i = 0; i < NUM_CLIENTS; i++)
		{
			Car *car = clients[i].car;
			if (SDL_LockMutex(clients[i].cmd_lock) == 0)
			{
				unsigned cmd = clients[i].cmd;
				if (cmd & NET_INPUT_UP) {
					car_accelerate(car);
				}
				if (cmd & NET_INPUT_DOWN) {
					car_decelerate(car);
				}
				if (cmd & NET_INPUT_LEFT) {
					car_turn_left(car);
				}
				if (cmd & NET_INPUT_RIGHT) {
					car_turn_right(car);
				}
				if (cmd & NET_INPUT_SPACE) {
					car->drift = 1;
				}
				if (cmd & NET_INPUT_RETURN) {
					car_use_powerup(car);
				}
				// Clear cmd
//				clients[i].cmd = 0;
				SDL_UnlockMutex(clients[i].cmd_lock);
			}
		}

		do_render(ren);

		cJSON *state = cJSON_CreateObject(), *car_json;
		cJSON_AddItemToObject(state, "cars", car_json = cJSON_CreateArray());
		for (int i = 0; i < NUM_CLIENTS; i++)
		{
			cJSON_AddItemToArray(car_json, car_serialize(clients[i].car));
		}
		cJSON_AddItemToObject(state, "shells", shells_serialize());
		cJSON_AddItemToObject(state, "boxes", boxes_serialize());
		if (SDL_LockMutex(json_state_lock) == 0)
		{
			free(json_state);
			json_state = json_to_text(state);
			SDL_UnlockMutex(json_state_lock);
		}
		cJSON_Delete(state);

		SDL_RenderPresent(ren);

		// Limit framerate
		Uint32 time1 = SDL_GetTicks();
		if (time1 - time0 < FRAMETIME_MS)
		{
			SDL_Delay(FRAMETIME_MS - (time1-time0));
		}
		time0 = time1;
	}

	// Clean up
	SDL_AtomicSet(&net_listen, 0);
	for (int i = 0; i < NUM_CLIENTS; i++)
	{
		int t_status;
		SDL_WaitThread(clients[i].thr, &t_status);
		SDL_DestroyMutex(clients[i].cmd_lock);
		net_close(clients[i].fd);
	}
	net_close(sockfd);
	SDL_DestroyMutex(json_state_lock);
	map_destroy();
	return 0;
}


int run_client(SDL_Renderer *ren)
{
	int quit = 0, num_cars = 0;
	SDL_Event event;
	char state[MAX_JSON_SIZE];
	cJSON *root;

	/* Set up initial state */
	net_recv(sockfd, state, MAX_JSON_SIZE);
	root = cJSON_Parse(state);
	if (root == NULL)
		printf("Received invalid JSON: %s\n", cJSON_GetErrorPtr());
	else
	{
		cJSON *cur;
		cur = cJSON_GetObjectItem(root, "num_cars");
		num_cars = cur->valueint;
		/* TODO: map */
	}

	for (int i = 0; i < num_cars; i++)
		car_add();

	while (!quit) {
		while (SDL_PollEvent(&event)){
			//If user closes the window
			if (event.type == SDL_QUIT) {
				quit = 1;
			}
			//If user presses any key
			if (event.type == SDL_KEYDOWN) {
				switch (event.key.keysym.sym) {
				case SDLK_ESCAPE:
					quit = 1;
					break;
				}
			}
		}

		const Uint8 *keystates = SDL_GetKeyboardState(NULL);
		if (keystates[SDL_SCANCODE_UP]) net_set_input(NET_INPUT_UP);
		if (keystates[SDL_SCANCODE_DOWN]) net_set_input(NET_INPUT_DOWN);
		if (keystates[SDL_SCANCODE_LEFT]) net_set_input(NET_INPUT_LEFT);
		if (keystates[SDL_SCANCODE_RIGHT]) net_set_input(NET_INPUT_RIGHT);
		if (keystates[SDL_SCANCODE_SPACE]) net_set_input(NET_INPUT_SPACE);
		if (keystates[SDL_SCANCODE_RETURN]) net_set_input(NET_INPUT_RETURN);
		net_send_input(sockfd);

		net_recv(sockfd, state, MAX_JSON_SIZE);
		root = cJSON_Parse(state);
		if (root == NULL)
			printf("Received invalid JSON: %s\n", cJSON_GetErrorPtr());
		else
		{
			cJSON *cars = cJSON_GetObjectItem(root, "cars");
			for (int i = 0; cars != NULL && i < cJSON_GetArraySize(cars); i++)
			{
				cJSON *car = cJSON_GetArrayItem(cars, i);
				car_deserialize(car);
			}
			cJSON_Delete(root);
			root = NULL;
		}

		render_background();
		map_render(ren);

		SDL_RenderPresent(ren);
	}

	// Clean up
	SDL_AtomicSet(&net_listen, 0);
	map_destroy();
	return 0;
}

int run_local(SDL_Renderer *ren)
{
	Car *cars[3];
	cars[0] = car_add();
	cars[1] = car_add();
	cars[2] = car_add();

	int quit = 0;
	SDL_Event event;

	Uint32 time0 = SDL_GetTicks();

	while (!quit) {
		while (SDL_PollEvent(&event)){
			//If user closes the window
			if (event.type == SDL_QUIT) {
				quit = 1;
			}
			//If user presses any key
			if (event.type == SDL_KEYDOWN) {
				switch (event.key.keysym.sym) {
				case SDLK_ESCAPE:
					quit = 1;
					break;
				}
			}
		}
		const Uint8 *keystates = SDL_GetKeyboardState(NULL);
		Car *car = cars[0];
		if (keystates[SDL_SCANCODE_UP]) {
			car_accelerate(car);
		}
		if (keystates[SDL_SCANCODE_DOWN]) {
			car_decelerate(car);
		}
		if (keystates[SDL_SCANCODE_LEFT]) {
			car_turn_left(car);
		}
		if (keystates[SDL_SCANCODE_RIGHT]) {
			car_turn_right(car);
		}
		if (keystates[SDL_SCANCODE_COMMA]) {
			car->drift = 1;
		}
		if (keystates[SDL_SCANCODE_PERIOD]) {
			car_use_powerup(car);
		}

		car = cars[1];
		if (keystates[SDL_SCANCODE_W]) {
			car_accelerate(car);
		}
		if (keystates[SDL_SCANCODE_S]) {
			car_decelerate(car);
		}
		if (keystates[SDL_SCANCODE_A]) {
			car_turn_left(car);
		}
		if (keystates[SDL_SCANCODE_D]) {
			car_turn_right(car);
		}
		if (keystates[SDL_SCANCODE_C]) {
			car->drift = 1;
		}
		if (keystates[SDL_SCANCODE_V]) {
			car_use_powerup(car);
		}

		do_render(ren);

		SDL_RenderPresent(ren);

		// Limit framerate
		Uint32 time1 = SDL_GetTicks();
		if (time1 - time0 < FRAMETIME_MS)
		{
			SDL_Delay(FRAMETIME_MS - (time1-time0));
		}
		time0 = time1;
	}

	// Clean up
	map_destroy();
	return 0;
}

char *show_get_ip(SDL_Renderer *ren)
{
	sound_set_type(SOUND_MENU);
	SDL_Event event;
	int quit = 0;
	int pos = 0;
	char *address = calloc(16, sizeof(*address));
	while (!quit) {
		while (SDL_PollEvent(&event)){
			//If user closes the window
			if (event.type == SDL_QUIT) {
				free(address);
				return 0;
			}
			//If user presses any key
			if (event.type == SDL_KEYDOWN) {
				switch (event.key.keysym.sym) {
				case SDLK_RETURN:
					return address;
					break;
				case SDLK_ESCAPE:
					free(address);
					return 0;
				case SDLK_1:
					address[pos] = '1';
					pos++;
					break;
				case SDLK_2:
					address[pos] = '2';
					pos++;
					break;
				case SDLK_3:
					address[pos] = '3';
					pos++;
					break;
				case SDLK_4:
					address[pos] = '4';
					pos++;
					break;
				case SDLK_5:
					address[pos] = '5';
					pos++;
					break;
				case SDLK_6:
					address[pos] = '6';
					pos++;
					break;
				case SDLK_7:
					address[pos] = '7';
					pos++;
					break;
				case SDLK_8:
					address[pos] = '8';
					pos++;
					break;
				case SDLK_9:
					address[pos] = '9';
					pos++;
					break;
				case SDLK_0:
					address[pos] = '0';
					pos++;
					break;
				case SDLK_LEFT:
					pos--;
					break;
				case SDLK_RIGHT:
				case SDLK_SPACE:
					pos++;
					break;
				case SDLK_PERIOD:
					address[pos] = '.';
					pos++;
					break;
				case SDLK_BACKSPACE:
					memset(address, 0, 16);
					pos = 0;
					break;
				}
				if (pos < 0) pos = 0;
				pos %= 15;
			}
		}


		SDL_SetRenderDrawColor(ren, 0x0, 0x0, 0x0, 0xff);
		SDL_RenderClear(ren);

		                          //r    g     b     a
		SDL_SetRenderDrawColor(ren, 0x0, 0xff, 0xff, 0xff);


		SDL_Rect box;
		box.w = 32 * 15;
		box.h = 40;
		box.x = SCREEN_WIDTH / 2 - box.w / 2 - 2;
		box.y = SCREEN_HEIGHT / 2 - 32;
		SDL_RenderDrawRect(ren, &box);
		render_string(address, box.x + 2, box.y, 32);

		render_string("enter ip:", box.x, box.y - 40, 32);

		SDL_Rect line;
		line.x = box.x + pos * 32;
		line.y = box.y + 30;
		line.w = 32;
		line.h = 5;
		SDL_RenderFillRect(ren, &line);

		// fancy useless effect
		for (int i=1; i<SCREEN_WIDTH/2; i++) {
			Uint32 t = SDL_GetTicks() / 10.0;
			int x = i * 2;
			int y = sinf(t * ((i - SCREEN_WIDTH/4)/500.0 + 0.01)) * (SCREEN_HEIGHT/2) + SCREEN_HEIGHT / 2;
			SDL_RenderDrawPoint(ren, x, y);
		}

		SDL_RenderPresent(ren);
	}
	return 0;
}

void show_menu(SDL_Renderer *ren)
{
	SDL_Texture *image = ren_load_image("startscreen.bmp");
	sound_set_type(SOUND_MENU);
	SDL_Event event;
	SDL_Rect target;
	target.x = 0;
	target.y = 0;
	target.w = SCREEN_WIDTH;
	target.h = SCREEN_HEIGHT;
	int quit = 0;
	MenuChoice choice = 0;
	while (!quit) {
		while (SDL_PollEvent(&event)){
			//If user closes the window
			if (event.type == SDL_QUIT) {
				return;
			}
			//If user presses any key
			if (event.type == SDL_KEYDOWN) {
				switch (event.key.keysym.sym) {
				case SDLK_RETURN:
					quit = 1;
					break;
				case SDLK_DOWN:
					choice = (choice + 1) % (MENU_QUIT + 1);
					break;
				case SDLK_UP:
					if (choice == MENU_SERVER) {
						choice = MENU_QUIT;
					} else {
						choice--;
					}
					break;
				}
			}
		}


		SDL_RenderCopy(ren, image, 0, &target);

		                            //r    g     b     a
		SDL_SetRenderDrawColor(ren, 0xff, 0xff, 0xff, 0xff);
		SDL_Rect selection_rect;
		selection_rect.x = 544;
		selection_rect.y = 272 + choice * 32;
		selection_rect.h = 30;
		selection_rect.w = 400;
		SDL_RenderDrawRect(ren, &selection_rect);

		render_string("server mode", 550, 275 + 32 * 0, 22);
		render_string("client mode", 550, 275 + 32 * 1, 22);
		render_string("local mode",  550, 275 + 32 * 2, 22);
		render_string("quit",        550, 275 + 32 * 3, 22);

		const char *verstring = "version " REVISION;
		render_string(verstring, 50, 530, 22);

		SDL_RenderPresent(ren);
	}

	//TODO: make not annoying sound
	sound_set_type(SOUND_NONE);
	SDL_SetRenderDrawColor(ren, 0x0, 0x0, 0x0, 0xff);
	SDL_RenderClear(ren);
	printf("menu choice: %d\n", choice);
	switch (choice)
	{
		case 0:
			run_server(ren);
			break;
		case 1: {
			char *address = show_get_ip(ren);
			if (address) {
				printf("addy: %s\n", address);
				sockfd = net_start_client(address, NET_PORT);
			run_client(ren);
			}
			break;
		}
		case 2:
			run_local(ren);
			break;
		case 3:
		default:
			break;
	}
}

int main(int argc, char *argv[])
{
	debug_install_handler();
	printf("kartering " REVISION " launching...\n");
	srand(time(NULL));
	net_init();

	// Set up SDL
	if (SDL_Init(SDL_INIT_EVERYTHING) < 0) {
		printf("SDL init failed: %s\n", SDL_GetError());
		return 1;
	}
	SDL_Window *win = SDL_CreateWindow("The Kartering", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, SCREEN_WIDTH, SCREEN_HEIGHT, SDL_WINDOW_SHOWN);
	if (win == NULL){
		printf("SDL error creating window: %s\n", SDL_GetError());
		return 1;
	}
	SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
	if (ren == NULL){
		printf("SDL error while creating renderer: %s\n", SDL_GetError());
		return 1;
	}
	if (!renderer_init(ren)) {
		printf("unable to initialize custom rendering\n");
		return 1;
	}

	if (!map_init("map1.map")) {
		printf("unable to initialize map!\n");
		return 1;
	}
	if (!boxes_init()) {
		printf("unable to initialize box!\n");
		return 1;
	}
	if (!powerups_init()) {
		printf("unable to initialize powerups!\n");
		return 1;
	}
	sound_init();

	if (argc > 1) {
		if (strcmp(argv[1], "server") == 0)
		{
			if (argc != 3)
			{
				printf("Usage: %s server <port>\n", argv[0]);
				return 1;
			}
			run_server(ren);
		}
		else if (strcmp(argv[1], "client") == 0)
		{
			if (argc != 4)
			{
				printf("Usage: %s client <address> <port>\n", argv[0]);
				return 1;
			}
			if (strcmp(argv[2], "localhost") == 0)
				sockfd = net_start_client("127.0.0.1", atoi(argv[3]));
			else
				sockfd = net_start_client(argv[2], atoi(argv[3]));
			run_client(ren);
		}
		else if (strcmp(argv[1], "local") == 0)
		{
			if (argc != 2)
			{
				printf("Usage: %s local\n", argv[0]);
				return 1;
			}
			run_local(ren);
		}
		else
		{
			printf("Invalid argument: %s\n", argv[1]);
			return 1;
		}
	} else {
		show_menu(ren);
	}

	if (sockfd != -1)
		net_cleanup();
	sound_destroy();
	SDL_DestroyRenderer(ren); // cleans up all textures
	SDL_DestroyWindow(win);
	SDL_Quit();
	return 0;
}

/* vim: set ts=8 sw=8 tw=0 noexpandtab cindent softtabstop=8 :*/
