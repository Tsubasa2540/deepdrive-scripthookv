/*
	THIS FILE IS A PART OF GTA V SCRIPT HOOK SDK
				http://dev-c.com			
			(C) Alexander Blade 2015
*/

/*
	F4					activate
	NUM2/8/4/6			navigate thru the menus and lists (numlock must be on)
	NUM5 				select
	NUM0/BACKSPACE/F4 	back
	NUM9/3 				use vehicle boost when active
	NUM+ 				use vehicle rockets when active
*/

// IS_POINT_ON_ROAD(float x, float y, float z, Entity entity) // 125BF4ABFC536B09 CF198055

#include "script.h"
#include "keyboard.h"
#include <string>
#include <ctime>

#pragma warning(disable : 4244 4305) // double <-> float conversions
#include <fstream>

#include <d3d11.h>
#pragma comment (lib, "d3d11.lib")
#include "GraphicsCaptureHook.h"
#include <set>
#include <sys/stat.h>

#include "DirectXMath.h"

#define _USE_MATH_DEFINES // for C
#include <math.h>

// thread example
#include <thread>         // std::thread
#include <vector>
#include <random>

int activeLineIndexVeh = 0;
DWORD lidarCam = NULL;
int vehicle = -1;
Ped playerPed;

bool shouldRunAlignCam = false;
bool shouldSelfDrive = false;


thread *alignCameraThread;

#define SafeRelease(var) if(var) {var->Release(); var = NULL;}

float * getDepthsAsFloats(unsigned height, unsigned width, BYTE* depths);

std::ofstream logFile;

ID3D11RenderTargetView *cq_backbuffer;    // cq the pointer to our back buffer
ID3D11DepthStencilView *cq_zbuffer;       // cq the pointer to our depth buffer

ID3D11Resource  *cq_pDepthBuffer;         // cq trying to get at depth buffer with CPU / CUDA
ID3D11Texture2D *cq_pDepthBufferCopy;     // cq trying to get at depth buffer with CPU / CUDA
set<ID3D11Resource*> cq_depthResources;
void copyDepthResources(IDXGISwapChain *swap);

int cq_height;
int cq_width;

std::random_device random_seed;     // only used once to initialise (seed) engine
std::mt19937 random_generator(random_seed());    // random-number engine used (Mersenne-Twister in this case)

inline int get_random_int(int start, int end)
{
	std::random_device rd;     // only used once to initialise (seed) engine
	std::uniform_int_distribution<int> uni(start, end); // guaranteed unbiased

	auto random_integer = uni(random_generator);

	return random_integer;
}

template<typename Iter, typename RandomGenerator>
Iter select_randomly(Iter start, Iter end, RandomGenerator& g) {
	std::uniform_int_distribution<> dis(0, std::distance(start, end) - 1);
	std::advance(start, dis(g));
	return start;
}

template<typename Iter>
Iter select_randomly(Iter start, Iter end) {
	static std::random_device rd;
	return select_randomly(start, end, random_generator);
}

inline double get_random_double(double start, double end)
{
	std::uniform_real_distribution<double> uni(start, end); // guaranteed unbiased
	auto random_double = uni(random_generator);
	return random_double;
}

// Reward shared memory

#define REWARD_SHARED_MEMORY TEXT("Local\\AgentReward")

struct SharedRewardData
{
	double distance;
	bool on_road;
	bool should_reset_agent;
	double heading;
	double speed;
	double desired_spin; // for directly setting spin, intermediate step to real control
	double desired_speed; // for directly setting speed, intermediate step to real control
	double desired_speed_change; // for directly setting speed change, intermediate step to real control
	double desired_direction;
	double spin;
	bool should_game_drive;
	bool should_perform_random_action;
	bool is_game_driving;
	int temp_action;
};

HANDLE rewardFileMap;
LPBYTE lpRewardSharedMemory = NULL;

void InitializeSharedRewardMemory(SharedRewardData **rewardData)
{
    int totalSize = sizeof(SharedRewardData);

	std::stringstream strName;
    strName << REWARD_SHARED_MEMORY;
    rewardFileMap = CreateFileMapping(INVALID_HANDLE_VALUE, NULL, 
		PAGE_READWRITE, 0, totalSize, strName.str().c_str());

    if(!rewardFileMap)
    {
	    return;
    }

    lpRewardSharedMemory = static_cast<LPBYTE>(
		MapViewOfFile(rewardFileMap, FILE_MAP_ALL_ACCESS, 0, 0, totalSize));

    if(!lpRewardSharedMemory)
    {
        CloseHandle(rewardFileMap);
        rewardFileMap = NULL;
		return;
    }

    *rewardData = reinterpret_cast<SharedRewardData*>(lpRewardSharedMemory);
    (*rewardData)->should_reset_agent = true;
    (*rewardData)->desired_speed = -8192;
    (*rewardData)->desired_spin = -8192;

}

void DestroyRewardSharedMemory()
{
    if(lpRewardSharedMemory && rewardFileMap)
    {
        UnmapViewOfFile(lpRewardSharedMemory);
        CloseHandle(rewardFileMap);

        rewardFileMap = NULL;
        lpRewardSharedMemory = NULL;
    }
}

// end reward shared memory

void saveDepthResource(ID3D11Resource* pResource)
{
	cq_depthResources.insert(pResource);
}

inline bool file_exists (const std::string& name) {
  struct stat buffer;   
  return (stat (name.c_str(), &buffer) == 0); 
}

std::string depthFileName = "cqDepthFloat.txt";
void copyDepth(IDXGISwapChain* swap, ID3D11Resource * cq_savedDepthResource, string filename)
{
	ID3D11Device* device = NULL;
	HRESULT hRes = swap->GetDevice(__uuidof(ID3D11Device), (void**)&device);

	ID3D11DeviceContext *devcon;
	device->GetImmediateContext(&devcon);

	// create the depth buffer copy texture
	D3D11_TEXTURE2D_DESC texdCopy;
	ZeroMemory(&texdCopy, sizeof(texdCopy));

	texdCopy.Width = cq_width;
	texdCopy.Height = cq_height;
	texdCopy.ArraySize = 1;
	texdCopy.MipLevels = 1;
	texdCopy.SampleDesc.Count = 1; // Must have BindFlags set if greater than 1.
	texdCopy.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS; 
	texdCopy.BindFlags = NULL;
	texdCopy.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	texdCopy.Usage = D3D11_USAGE_STAGING; // Must be staging to read from CPU, but means BindFlags cannot be set.

	device->CreateTexture2D(&texdCopy, NULL, &cq_pDepthBufferCopy);

	devcon->CopyResource(cq_pDepthBufferCopy, cq_savedDepthResource);

	D3D11_MAPPED_SUBRESOURCE mappedResource;

	HRESULT mapResult = devcon->Map(cq_pDepthBufferCopy, 0, D3D11_MAP_READ, 0, &mappedResource);

	BYTE* pYourBytes = (BYTE*)mappedResource.pData;
	unsigned int uiPitch = mappedResource.RowPitch;

	float * fDepths = getDepthsAsFloats(cq_height, cq_width, pYourBytes);

	std::ofstream myfile;
	myfile.open(filename);
	auto floatWidth = cq_height * cq_width;
	for(auto i = 0; i < floatWidth; i++)
	{
		myfile << fDepths[i] << " ";
	}
	myfile.close();

	devcon->Unmap(cq_pDepthBufferCopy, 0);
	devcon->Release();
	device->Release();	
}

void copyDepthResources(IDXGISwapChain *swap)
{
	auto i = 0;
	for (auto &resource : cq_depthResources)
	{
		string filename = depthFileName + to_string(i);
		if( file_exists(filename) == false )
		{
			copyDepth(swap, resource, filename);
		}
		i++;
	}
}

void draw_rect(float A_0, float A_1, float A_2, float A_3, int A_4, int A_5, int A_6, int A_7)
{
	GRAPHICS::DRAW_RECT((A_0 + (A_2 * 0.5f)), (A_1 + (A_3 * 0.5f)), A_2, A_3, A_4, A_5, A_6, A_7);
}

void draw_menu_line(std::string caption, float lineWidth, float lineHeight, float lineTop, float lineLeft, float textLeft, bool active, bool title, bool rescaleText = true)
{
	// default values
	int text_col[4] = {255, 255, 255, 255},
		rect_col[4] = {70, 95, 95, 255};
	float text_scale = 0.35;
	int font = 0;

	// correcting values for active line
	if (active)
	{
		text_col[0] = 0;
		text_col[1] = 0;
		text_col[2] = 0;

		rect_col[0] = 218;
		rect_col[1] = 242;
		rect_col[2] = 216;

		if (rescaleText) text_scale = 0.40;
	}

	if (title)
	{
		rect_col[0] = 0;
		rect_col[1] = 0;
		rect_col[2] = 0;

		if (rescaleText) text_scale = 0.50;
		font = 1;
	}

	int screen_w, screen_h;
	GRAPHICS::GET_SCREEN_RESOLUTION(&screen_w, &screen_h);

	textLeft += lineLeft;

	float lineWidthScaled = lineWidth / (float)screen_w; // line width
	float lineTopScaled = lineTop / (float)screen_h; // line top offset
	float textLeftScaled = textLeft / (float)screen_w; // text left offset
	float lineHeightScaled = lineHeight / (float)screen_h; // line height

	float lineLeftScaled = lineLeft / (float)screen_w;

	// this is how it's done in original scripts

	// text upper part
	UI::SET_TEXT_FONT(font);
	UI::SET_TEXT_SCALE(0.0, text_scale);
	UI::SET_TEXT_COLOUR(text_col[0], text_col[1], text_col[2], text_col[3]);
	UI::SET_TEXT_CENTRE(0);
	UI::SET_TEXT_DROPSHADOW(0, 0, 0, 0, 0);
	UI::SET_TEXT_EDGE(0, 0, 0, 0, 0);
	UI::_SET_TEXT_ENTRY("STRING");
	UI::_ADD_TEXT_COMPONENT_STRING((LPSTR)caption.c_str());
	UI::_DRAW_TEXT(textLeftScaled, (((lineTopScaled + 0.00278f) + lineHeightScaled) - 0.005f));

	// text lower part
	UI::SET_TEXT_FONT(font);
	UI::SET_TEXT_SCALE(0.0, text_scale);
	UI::SET_TEXT_COLOUR(text_col[0], text_col[1], text_col[2], text_col[3]);
	UI::SET_TEXT_CENTRE(0);
	UI::SET_TEXT_DROPSHADOW(0, 0, 0, 0, 0);
	UI::SET_TEXT_EDGE(0, 0, 0, 0, 0);
	UI::_SET_TEXT_GXT_ENTRY("STRING");
	UI::_ADD_TEXT_COMPONENT_STRING((LPSTR)caption.c_str());
	int num25 = UI::_0x9040DFB09BE75706(textLeftScaled, (((lineTopScaled + 0.00278f) + lineHeightScaled) - 0.005f));

	// rect
	draw_rect(lineLeftScaled, lineTopScaled + (0.00278f), 
		lineWidthScaled, ((((float)(num25)* UI::_0xDB88A37483346780(text_scale, 0)) + (lineHeightScaled * 2.0f)) + 0.005f),
		rect_col[0], rect_col[1], rect_col[2], rect_col[3]);	
}

bool trainer_switch_pressed()
{
	return IsKeyJustUp(VK_F4);
}

void get_button_state(bool *a, bool *b, bool *up, bool *down, bool *l, bool *r)
{
	if (a) *a = IsKeyDown(VK_NUMPAD5);
	if (b) *b = IsKeyDown(VK_NUMPAD0) || trainer_switch_pressed() || IsKeyDown(VK_BACK);
	if (up) *up = IsKeyDown(VK_NUMPAD8);
	if (down) *down = IsKeyDown(VK_NUMPAD2);
	if (r) *r = IsKeyDown(VK_NUMPAD6);
	if (l) *l = IsKeyDown(VK_NUMPAD4);
}

void menu_beep()
{
	AUDIO::PLAY_SOUND_FRONTEND(-1, "NAV_UP_DOWN", "HUD_FRONTEND_DEFAULT_SOUNDSET", 0);
}

std::string statusText;
DWORD statusTextDrawTicksMax;
bool statusTextGxtEntry;

void update_status_text()
{
	if (GetTickCount() < statusTextDrawTicksMax)
	{
		UI::SET_TEXT_FONT(0);
		UI::SET_TEXT_SCALE(0.55, 0.55);
		UI::SET_TEXT_COLOUR(255, 255, 255, 255);
		UI::SET_TEXT_WRAP(0.0, 1.0);
		UI::SET_TEXT_CENTRE(1);
		UI::SET_TEXT_DROPSHADOW(0, 0, 0, 0, 0);
		UI::SET_TEXT_EDGE(1, 0, 0, 0, 205);
		if (statusTextGxtEntry)
		{
			UI::_SET_TEXT_ENTRY((char *)statusText.c_str());
		} else
		{
			UI::_SET_TEXT_ENTRY("STRING");
			UI::_ADD_TEXT_COMPONENT_STRING((char *)statusText.c_str());
		}
		UI::_DRAW_TEXT(0.5, 0.5);
	}
}

void set_status_text(std::string str, DWORD time = 2500, bool isGxtEntry = false)
{
	statusText = str;
	statusTextDrawTicksMax = GetTickCount() + time;
	statusTextGxtEntry = isGxtEntry;
}

// features
bool featurePlayerInvincible			=	false;
bool featurePlayerInvincibleUpdated		=	false;
bool featurePlayerNeverWanted			=	false;
bool featurePlayerIgnored				=	false;
bool featurePlayerIgnoredUpdated		=	false;
bool featurePlayerUnlimitedAbility		=	false;
bool featurePlayerNoNoise				=	false;
bool featurePlayerNoNoiseUpdated		=	false;
bool featurePlayerFastSwim				=	false;
bool featurePlayerFastSwimUpdated		=	false;
bool featurePlayerFastRun				=	false;
bool featurePlayerFastRunUpdated		=	false;
bool featurePlayerSuperJump				=	false;

bool featureWeaponNoReload				=	false;
bool featureWeaponFireAmmo				=	false;
bool featureWeaponExplosiveAmmo			=	false;
bool featureWeaponExplosiveMelee		=	false;
bool featureWeaponVehRockets			=	false;

DWORD featureWeaponVehShootLastTime		=	0;

bool featureVehInvincible				=	false;
bool featureVehInvincibleUpdated		=	false;
bool featureVehInvincibleWheels			=	false;
bool featureVehInvincibleWheelsUpdated	=	false;
bool featureVehSeatbelt					=	false;
bool featureVehSeatbeltUpdated			=	false;
bool featureVehSpeedBoost				=	false;
bool featureVehShowStats				=	false;
bool featureVehShouldGameDrive	        =	false;
bool featureVehWrapInSpawned			=	false;

bool featureWorldMoonGravity			=	false;
bool featureWorldRandomCops				=	true;
bool featureWorldRandomTrains			=	true;
bool featureWorldRandomBoats			=	true;
bool featureWorldGarbageTrucks			=	true;

bool featureTimePaused					=	false;
bool featureTimePausedUpdated			=	false;
bool featureTimeSynced					=	false;

bool featureWeatherWind					=	false;
bool featureWeatherPers					=	false;

bool featureMiscLockRadio				=	false;
bool featureMiscHideHud					=	false;


// player model control, switching on normal ped model when needed	
void check_player_model() 
{
	// common variables
	Player player = PLAYER::PLAYER_ID();
	Ped playerPed = PLAYER::PLAYER_PED_ID();	

	if (!ENTITY::DOES_ENTITY_EXIST(playerPed)) return;

	Hash model = ENTITY::GET_ENTITY_MODEL(playerPed);
	if (ENTITY::IS_ENTITY_DEAD(playerPed) || PLAYER::IS_PLAYER_BEING_ARRESTED(player, TRUE))
		if (model != GAMEPLAY::GET_HASH_KEY("player_zero") && 
			model != GAMEPLAY::GET_HASH_KEY("player_one") &&
			model != GAMEPLAY::GET_HASH_KEY("player_two"))
		{
			set_status_text("turning to normal");
			WAIT(1000);

			model = GAMEPLAY::GET_HASH_KEY("player_zero");
			STREAMING::REQUEST_MODEL(model);
			while (!STREAMING::HAS_MODEL_LOADED(model))
				WAIT(0);
			PLAYER::SET_PLAYER_MODEL(PLAYER::PLAYER_ID(), model);
			PED::SET_PED_DEFAULT_COMPONENT_VARIATION(PLAYER::PLAYER_PED_ID());
			STREAMING::SET_MODEL_AS_NO_LONGER_NEEDED(model);

			// wait until player is ressurected
			while (ENTITY::IS_ENTITY_DEAD(PLAYER::PLAYER_PED_ID()) || PLAYER::IS_PLAYER_BEING_ARRESTED(player, TRUE))
				WAIT(0);

		}
}

void update_vehicle_guns()
{
	Player player = PLAYER::PLAYER_ID();
	Ped playerPed = PLAYER::PLAYER_PED_ID();	

	if (!ENTITY::DOES_ENTITY_EXIST(playerPed) || !featureWeaponVehRockets) return;

	bool bSelect = IsKeyDown(0x6B); // num plus
	if (bSelect && featureWeaponVehShootLastTime + 150 < GetTickCount() &&
		PLAYER::IS_PLAYER_CONTROL_ON(player) &&	PED::IS_PED_IN_ANY_VEHICLE(playerPed, 0))
	{
		Vehicle veh = PED::GET_VEHICLE_PED_IS_USING(playerPed);

		Vector3 v0, v1;
		GAMEPLAY::GET_MODEL_DIMENSIONS(ENTITY::GET_ENTITY_MODEL(veh), &v0, &v1);

		Hash weaponAssetRocket = GAMEPLAY::GET_HASH_KEY("WEAPON_VEHICLE_ROCKET");
		if (!WEAPON::HAS_WEAPON_ASSET_LOADED(weaponAssetRocket))
		{
			WEAPON::REQUEST_WEAPON_ASSET(weaponAssetRocket, 31, 0);
			while (!WEAPON::HAS_WEAPON_ASSET_LOADED(weaponAssetRocket))
				WAIT(0);
		}

		Vector3 coords0from = ENTITY::GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS(veh, -(v1.x + 0.25f), v1.y + 1.25f, 0.1);
		Vector3 coords1from = ENTITY::GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS(veh,  (v1.x + 0.25f), v1.y + 1.25f, 0.1);
		Vector3 coords0to = ENTITY::GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS(veh, -v1.x, v1.y + 100.0f, 0.1f);
		Vector3 coords1to = ENTITY::GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS(veh,  v1.x, v1.y + 100.0f, 0.1f);

		GAMEPLAY::SHOOT_SINGLE_BULLET_BETWEEN_COORDS(coords0from.x, coords0from.y, coords0from.z, 
													 coords0to.x, coords0to.y, coords0to.z, 
													 250, 1, weaponAssetRocket, playerPed, 1, 0, -1.0);
		GAMEPLAY::SHOOT_SINGLE_BULLET_BETWEEN_COORDS(coords1from.x, coords1from.y, coords1from.z, 
													 coords1to.x, coords1to.y, coords1to.z, 
													 250, 1, weaponAssetRocket, playerPed, 1, 0, -1.0);

		featureWeaponVehShootLastTime = GetTickCount();
	}
}

bool skinchanger_used = false;

// Updates all features that can be turned off by the game, being called each game frame
void update_features() 
{
	update_status_text();

	update_vehicle_guns();

	// changing player model if died/arrested while being in another skin, since it can cause inf loading loop
	if (skinchanger_used)
		check_player_model();

	// wait until player is ready, basicly to prevent using the trainer while player is dead or arrested
	while (ENTITY::IS_ENTITY_DEAD(PLAYER::PLAYER_PED_ID()) || PLAYER::IS_PLAYER_BEING_ARRESTED(PLAYER::PLAYER_ID(), TRUE))
		WAIT(0);

	// read default feature values from the game
	featureWorldRandomCops = PED::CAN_CREATE_RANDOM_COPS() == TRUE;

	// common variables
	Player player = PLAYER::PLAYER_ID();
	Ped playerPed = PLAYER::PLAYER_PED_ID();	
	BOOL bPlayerExists = ENTITY::DOES_ENTITY_EXIST(playerPed);

	// player invincible
	if (featurePlayerInvincibleUpdated)
	{
		if (bPlayerExists && !featurePlayerInvincible)
			PLAYER::SET_PLAYER_INVINCIBLE(player, FALSE);
		featurePlayerInvincibleUpdated = false;
	}
	if (featurePlayerInvincible)
	{
		if (bPlayerExists)
			PLAYER::SET_PLAYER_INVINCIBLE(player, TRUE);
	}

	// player never wanted
	if (featurePlayerNeverWanted)
	{
		if (bPlayerExists)
			PLAYER::CLEAR_PLAYER_WANTED_LEVEL(player);
	}

	// police ignore player
	if (featurePlayerIgnoredUpdated)
	{
		if (bPlayerExists)
			PLAYER::SET_POLICE_IGNORE_PLAYER(player, featurePlayerIgnored);
		featurePlayerIgnoredUpdated = false;
	}

	// player special ability
	if (featurePlayerUnlimitedAbility)
	{
		if (bPlayerExists)
			PLAYER::_RECHARGE_SPECIAL_ABILITY(player, 1);
	}

	// player no noise
	if (featurePlayerNoNoiseUpdated)
	{
		if (bPlayerExists && !featurePlayerNoNoise)
			PLAYER::SET_PLAYER_NOISE_MULTIPLIER(player, 1.0);
		featurePlayerNoNoiseUpdated = false;
	}
	if (featurePlayerNoNoise)
		PLAYER::SET_PLAYER_NOISE_MULTIPLIER(player, 0.0);

	// player fast swim
	if (featurePlayerFastSwimUpdated)
	{
		if (bPlayerExists && !featurePlayerFastSwim)
			PLAYER::_SET_SWIM_SPEED_MULTIPLIER(player, 1.0);
		featurePlayerFastSwimUpdated = false;
	}
	if (featurePlayerFastSwim)
		PLAYER::_SET_SWIM_SPEED_MULTIPLIER(player, 1.49);

	// player fast run
	if (featurePlayerFastRunUpdated)
	{
		if (bPlayerExists && !featurePlayerFastRun)
			PLAYER::_SET_MOVE_SPEED_MULTIPLIER(player, 1.0);
		featurePlayerFastRunUpdated = false;
	}
	if (featurePlayerFastRun)
		PLAYER::_SET_MOVE_SPEED_MULTIPLIER(player, 1.49);

	// player super jump
	if (featurePlayerSuperJump)
	{
		if (bPlayerExists)
			GAMEPLAY::SET_SUPER_JUMP_THIS_FRAME(player);
	}

	// weapon
	if (featureWeaponFireAmmo)
	{
		if (bPlayerExists)
			GAMEPLAY::SET_FIRE_AMMO_THIS_FRAME(player);
	}
	if (featureWeaponExplosiveAmmo)
	{
		if (bPlayerExists)
			GAMEPLAY::SET_EXPLOSIVE_AMMO_THIS_FRAME(player);
	}
	if (featureWeaponExplosiveMelee)
	{
		if (bPlayerExists)
			GAMEPLAY::SET_EXPLOSIVE_MELEE_THIS_FRAME(player);
	}

	// weapon no reload
	if (bPlayerExists && featureWeaponNoReload)
	{
		Hash cur;
		if (WEAPON::GET_CURRENT_PED_WEAPON(playerPed, &cur, 1))
		{
			if (WEAPON::IS_WEAPON_VALID(cur))
			{
				int maxAmmo;
				if (WEAPON::GET_MAX_AMMO(playerPed, cur, &maxAmmo))
				{
					WEAPON::SET_PED_AMMO(playerPed, cur, maxAmmo);

					maxAmmo = WEAPON::GET_MAX_AMMO_IN_CLIP(playerPed, cur, 1);
					if (maxAmmo > 0)
						WEAPON::SET_AMMO_IN_CLIP(playerPed, cur, maxAmmo);
				}
			}
		}
	}

	// player's vehicle invincible
	if (featureVehInvincibleUpdated)
	{
		if (bPlayerExists && !featureVehInvincible && PED::IS_PED_IN_ANY_VEHICLE(playerPed, 0))
		{
			Vehicle veh = PED::GET_VEHICLE_PED_IS_USING(playerPed);
			ENTITY::SET_ENTITY_INVINCIBLE(veh, FALSE);
			ENTITY::SET_ENTITY_PROOFS(veh, 0, 0, 0, 0, 0, 0, 0, 0);
			VEHICLE::SET_VEHICLE_TYRES_CAN_BURST(veh, 1);
			VEHICLE::SET_VEHICLE_WHEELS_CAN_BREAK(veh, 1);
			VEHICLE::SET_VEHICLE_CAN_BE_VISIBLY_DAMAGED(veh, 1);
		}
		featureVehInvincibleUpdated = false;
	}
	if (featureVehInvincible)
	{
		if (bPlayerExists && PED::IS_PED_IN_ANY_VEHICLE(playerPed, 0))
		{
			Vehicle veh = PED::GET_VEHICLE_PED_IS_USING(playerPed);
			ENTITY::SET_ENTITY_INVINCIBLE(veh, TRUE);
			ENTITY::SET_ENTITY_PROOFS(veh, 1, 1, 1, 1, 1, 1, 1, 1);	
			VEHICLE::SET_VEHICLE_TYRES_CAN_BURST(veh, 0);
			VEHICLE::SET_VEHICLE_WHEELS_CAN_BREAK(veh, 0);
			VEHICLE::SET_VEHICLE_CAN_BE_VISIBLY_DAMAGED(veh, 0);
		}
	}

	// player's vehicle invincible wheels, usefull with custom handling
	if (featureVehInvincibleWheelsUpdated)
	{
		if (bPlayerExists && !featureVehInvincibleWheels && PED::IS_PED_IN_ANY_VEHICLE(playerPed, 0))
		{
			Vehicle veh = PED::GET_VEHICLE_PED_IS_USING(playerPed);
			VEHICLE::SET_VEHICLE_TYRES_CAN_BURST(veh, TRUE);
			VEHICLE::SET_VEHICLE_WHEELS_CAN_BREAK(veh, TRUE);
			VEHICLE::SET_VEHICLE_HAS_STRONG_AXLES(veh, FALSE);
		}
		featureVehInvincibleWheelsUpdated = false;
	}
	if (featureVehInvincibleWheels)
	{
		if (bPlayerExists && PED::IS_PED_IN_ANY_VEHICLE(playerPed, 0))
		{
			Vehicle veh = PED::GET_VEHICLE_PED_IS_USING(playerPed);
			VEHICLE::SET_VEHICLE_TYRES_CAN_BURST(veh, FALSE);
			VEHICLE::SET_VEHICLE_WHEELS_CAN_BREAK(veh, FALSE);
			VEHICLE::SET_VEHICLE_HAS_STRONG_AXLES(veh, TRUE);
		}
	}

	// seat belt
	const int PED_FLAG_CAN_FLY_THRU_WINDSCREEN = 32;
	if (featureVehSeatbeltUpdated)
	{
		if (bPlayerExists && !featureVehSeatbelt)
			PED::SET_PED_CONFIG_FLAG(playerPed, PED_FLAG_CAN_FLY_THRU_WINDSCREEN, TRUE);
		featureVehSeatbeltUpdated = false;
	}
	if (featureVehSeatbelt)
	{
		if (bPlayerExists)
		{
			if (PED::GET_PED_CONFIG_FLAG(playerPed, PED_FLAG_CAN_FLY_THRU_WINDSCREEN, TRUE))
				PED::SET_PED_CONFIG_FLAG(playerPed, PED_FLAG_CAN_FLY_THRU_WINDSCREEN, FALSE);
		}
	}

	// player's vehicle boost
	if (featureVehSpeedBoost && bPlayerExists && PED::IS_PED_IN_ANY_VEHICLE(playerPed, 0))
	{
		Vehicle veh = PED::GET_VEHICLE_PED_IS_USING(playerPed);
		DWORD model = ENTITY::GET_ENTITY_MODEL(veh);

		bool bUp = IsKeyDown(VK_NUMPAD9);
		bool bDown = IsKeyDown(VK_NUMPAD3);

		if (bUp || bDown)
		{			
			float speed = ENTITY::GET_ENTITY_SPEED(veh);
			if (bUp) 
			{
				if (speed < 3.0f) speed = 3.0f;
				speed += speed * 0.05f;
				VEHICLE::SET_VEHICLE_FORWARD_SPEED(veh, speed);
			} else
			if (ENTITY::IS_ENTITY_IN_AIR(veh) || speed > 5.0) 
				VEHICLE::SET_VEHICLE_FORWARD_SPEED(veh, 0.0);
		}
	}

	// time pause
	if (featureTimePausedUpdated)
	{
		TIME::PAUSE_CLOCK(featureTimePaused);
		featureTimePausedUpdated = false;
	}

	// time sync
	if (featureTimeSynced)
	{
		time_t now = time(0);
		tm t;
		localtime_s(&t, &now);
		TIME::SET_CLOCK_TIME(t.tm_hour, t.tm_min, t.tm_sec);
	}

	// hide hud
	if (featureMiscHideHud)
		UI::HIDE_HUD_AND_RADAR_THIS_FRAME();
}

LPCSTR pedModels[69][10] = {
	{"player_zero", "player_one", "player_two", "a_c_boar", "a_c_chimp", "a_c_cow", "a_c_coyote", "a_c_deer", "a_c_fish", "a_c_hen"},
	{ "a_c_cat_01", "a_c_chickenhawk", "a_c_cormorant", "a_c_crow", "a_c_dolphin", "a_c_humpback", "a_c_killerwhale", "a_c_pigeon", "a_c_seagull", "a_c_sharkhammer"},
	{"a_c_pig", "a_c_rat", "a_c_rhesus", "a_c_chop", "a_c_husky", "a_c_mtlion", "a_c_retriever", "a_c_sharktiger", "a_c_shepherd", "s_m_m_movalien_01"},
	{"a_f_m_beach_01", "a_f_m_bevhills_01", "a_f_m_bevhills_02", "a_f_m_bodybuild_01", "a_f_m_business_02", "a_f_m_downtown_01", "a_f_m_eastsa_01", "a_f_m_eastsa_02", "a_f_m_fatbla_01", "a_f_m_fatcult_01"},
	{"a_f_m_fatwhite_01", "a_f_m_ktown_01", "a_f_m_ktown_02", "a_f_m_prolhost_01", "a_f_m_salton_01", "a_f_m_skidrow_01", "a_f_m_soucentmc_01", "a_f_m_soucent_01", "a_f_m_soucent_02", "a_f_m_tourist_01"},
	{"a_f_m_trampbeac_01", "a_f_m_tramp_01", "a_f_o_genstreet_01", "a_f_o_indian_01", "a_f_o_ktown_01", "a_f_o_salton_01", "a_f_o_soucent_01", "a_f_o_soucent_02", "a_f_y_beach_01", "a_f_y_bevhills_01"},
	{"a_f_y_bevhills_02", "a_f_y_bevhills_03", "a_f_y_bevhills_04", "a_f_y_business_01", "a_f_y_business_02", "a_f_y_business_03", "a_f_y_business_04", "a_f_y_eastsa_01", "a_f_y_eastsa_02", "a_f_y_eastsa_03"},
	{"a_f_y_epsilon_01", "a_f_y_fitness_01", "a_f_y_fitness_02", "a_f_y_genhot_01", "a_f_y_golfer_01", "a_f_y_hiker_01", "a_f_y_hippie_01", "a_f_y_hipster_01", "a_f_y_hipster_02", "a_f_y_hipster_03"},
	{"a_f_y_hipster_04", "a_f_y_indian_01", "a_f_y_juggalo_01", "a_f_y_runner_01", "a_f_y_rurmeth_01", "a_f_y_scdressy_01", "a_f_y_skater_01", "a_f_y_soucent_01", "a_f_y_soucent_02", "a_f_y_soucent_03"},
	{"a_f_y_tennis_01", "a_f_y_topless_01", "a_f_y_tourist_01", "a_f_y_tourist_02", "a_f_y_vinewood_01", "a_f_y_vinewood_02", "a_f_y_vinewood_03", "a_f_y_vinewood_04", "a_f_y_yoga_01", "a_m_m_acult_01"},
	{"a_m_m_afriamer_01", "a_m_m_beach_01", "a_m_m_beach_02", "a_m_m_bevhills_01", "a_m_m_bevhills_02", "a_m_m_business_01", "a_m_m_eastsa_01", "a_m_m_eastsa_02", "a_m_m_farmer_01", "a_m_m_fatlatin_01"},
	{"a_m_m_genfat_01", "a_m_m_genfat_02", "a_m_m_golfer_01", "a_m_m_hasjew_01", "a_m_m_hillbilly_01", "a_m_m_hillbilly_02", "a_m_m_indian_01", "a_m_m_ktown_01", "a_m_m_malibu_01", "a_m_m_mexcntry_01"},
	{"a_m_m_mexlabor_01", "a_m_m_og_boss_01", "a_m_m_paparazzi_01", "a_m_m_polynesian_01", "a_m_m_prolhost_01", "a_m_m_rurmeth_01", "a_m_m_salton_01", "a_m_m_salton_02", "a_m_m_salton_03", "a_m_m_salton_04"},
	{"a_m_m_skater_01", "a_m_m_skidrow_01", "a_m_m_socenlat_01", "a_m_m_soucent_01", "a_m_m_soucent_02", "a_m_m_soucent_03", "a_m_m_soucent_04", "a_m_m_stlat_02", "a_m_m_tennis_01", "a_m_m_tourist_01"},
	{"a_m_m_trampbeac_01", "a_m_m_tramp_01", "a_m_m_tranvest_01", "a_m_m_tranvest_02", "a_m_o_acult_01", "a_m_o_acult_02", "a_m_o_beach_01", "a_m_o_genstreet_01", "a_m_o_ktown_01", "a_m_o_salton_01"},
	{"a_m_o_soucent_01", "a_m_o_soucent_02", "a_m_o_soucent_03", "a_m_o_tramp_01", "a_m_y_acult_01", "a_m_y_acult_02", "a_m_y_beachvesp_01", "a_m_y_beachvesp_02", "a_m_y_beach_01", "a_m_y_beach_02"},
	{"a_m_y_beach_03", "a_m_y_bevhills_01", "a_m_y_bevhills_02", "a_m_y_breakdance_01", "a_m_y_busicas_01", "a_m_y_business_01", "a_m_y_business_02", "a_m_y_business_03", "a_m_y_cyclist_01", "a_m_y_dhill_01"},
	{"a_m_y_downtown_01", "a_m_y_eastsa_01", "a_m_y_eastsa_02", "a_m_y_epsilon_01", "a_m_y_epsilon_02", "a_m_y_gay_01", "a_m_y_gay_02", "a_m_y_genstreet_01", "a_m_y_genstreet_02", "a_m_y_golfer_01"},
	{"a_m_y_hasjew_01", "a_m_y_hiker_01", "a_m_y_hippy_01", "a_m_y_hipster_01", "a_m_y_hipster_02", "a_m_y_hipster_03", "a_m_y_indian_01", "a_m_y_jetski_01", "a_m_y_juggalo_01", "a_m_y_ktown_01"},
	{"a_m_y_ktown_02", "a_m_y_latino_01", "a_m_y_methhead_01", "a_m_y_mexthug_01", "a_m_y_motox_01", "a_m_y_motox_02", "a_m_y_musclbeac_01", "a_m_y_musclbeac_02", "a_m_y_polynesian_01", "a_m_y_roadcyc_01"},
	{"a_m_y_runner_01", "a_m_y_runner_02", "a_m_y_salton_01", "a_m_y_skater_01", "a_m_y_skater_02", "a_m_y_soucent_01", "a_m_y_soucent_02", "a_m_y_soucent_03", "a_m_y_soucent_04", "a_m_y_stbla_01"},
	{"a_m_y_stbla_02", "a_m_y_stlat_01", "a_m_y_stwhi_01", "a_m_y_stwhi_02", "a_m_y_sunbathe_01", "a_m_y_surfer_01", "a_m_y_vindouche_01", "a_m_y_vinewood_01", "a_m_y_vinewood_02", "a_m_y_vinewood_03"},
	{"a_m_y_vinewood_04", "a_m_y_yoga_01", "u_m_y_proldriver_01", "u_m_y_rsranger_01", "u_m_y_sbike", "u_m_y_staggrm_01", "u_m_y_tattoo_01", "csb_abigail", "csb_anita", "csb_anton"},
	{"csb_ballasog", "csb_bride", "csb_burgerdrug", "csb_car3guy1", "csb_car3guy2", "csb_chef", "csb_chin_goon", "csb_cletus", "csb_cop", "csb_customer"},
	{"csb_denise_friend", "csb_fos_rep", "csb_g", "csb_groom", "csb_grove_str_dlr", "csb_hao", "csb_hugh", "csb_imran", "csb_janitor", "csb_maude"},
	{"csb_mweather", "csb_ortega", "csb_oscar", "csb_porndudes", "csb_porndudes_p", "csb_prologuedriver", "csb_prolsec", "csb_ramp_gang", "csb_ramp_hic", "csb_ramp_hipster"},
	{"csb_ramp_marine", "csb_ramp_mex", "csb_reporter", "csb_roccopelosi", "csb_screen_writer", "csb_stripper_01", "csb_stripper_02", "csb_tonya", "csb_trafficwarden", "cs_amandatownley"},
	{"cs_andreas", "cs_ashley", "cs_bankman", "cs_barry", "cs_barry_p", "cs_beverly", "cs_beverly_p", "cs_brad", "cs_bradcadaver", "cs_carbuyer"},
	{"cs_casey", "cs_chengsr", "cs_chrisformage", "cs_clay", "cs_dale", "cs_davenorton", "cs_debra", "cs_denise", "cs_devin", "cs_dom"},
	{"cs_dreyfuss", "cs_drfriedlander", "cs_fabien", "cs_fbisuit_01", "cs_floyd", "cs_guadalope", "cs_gurk", "cs_hunter", "cs_janet", "cs_jewelass"},
	{"cs_jimmyboston", "cs_jimmydisanto", "cs_joeminuteman", "cs_johnnyklebitz", "cs_josef", "cs_josh", "cs_lamardavis", "cs_lazlow", "cs_lestercrest", "cs_lifeinvad_01"},
	{"cs_magenta", "cs_manuel", "cs_marnie", "cs_martinmadrazo", "cs_maryann", "cs_michelle", "cs_milton", "cs_molly", "cs_movpremf_01", "cs_movpremmale"},
	{"cs_mrk", "cs_mrsphillips", "cs_mrs_thornhill", "cs_natalia", "cs_nervousron", "cs_nigel", "cs_old_man1a", "cs_old_man2", "cs_omega", "cs_orleans"},
	{"cs_paper", "cs_paper_p", "cs_patricia", "cs_priest", "cs_prolsec_02", "cs_russiandrunk", "cs_siemonyetarian", "cs_solomon", "cs_stevehains", "cs_stretch"},
	{"cs_tanisha", "cs_taocheng", "cs_taostranslator", "cs_tenniscoach", "cs_terry", "cs_tom", "cs_tomepsilon", "cs_tracydisanto", "cs_wade", "cs_zimbor"},
	{"g_f_y_ballas_01", "g_f_y_families_01", "g_f_y_lost_01", "g_f_y_vagos_01", "g_m_m_armboss_01", "g_m_m_armgoon_01", "g_m_m_armlieut_01", "g_m_m_chemwork_01", "g_m_m_chemwork_01_p", "g_m_m_chiboss_01"},
	{"g_m_m_chiboss_01_p", "g_m_m_chicold_01", "g_m_m_chicold_01_p", "g_m_m_chigoon_01", "g_m_m_chigoon_01_p", "g_m_m_chigoon_02", "g_m_m_korboss_01", "g_m_m_mexboss_01", "g_m_m_mexboss_02", "g_m_y_armgoon_02"},
	{"g_m_y_azteca_01", "g_m_y_ballaeast_01", "g_m_y_ballaorig_01", "g_m_y_ballasout_01", "g_m_y_famca_01", "g_m_y_famdnf_01", "g_m_y_famfor_01", "g_m_y_korean_01", "g_m_y_korean_02", "g_m_y_korlieut_01"},
	{"g_m_y_lost_01", "g_m_y_lost_02", "g_m_y_lost_03", "g_m_y_mexgang_01", "g_m_y_mexgoon_01", "g_m_y_mexgoon_02", "g_m_y_mexgoon_03", "g_m_y_mexgoon_03_p", "g_m_y_pologoon_01", "g_m_y_pologoon_01_p"},
	{"g_m_y_pologoon_02", "g_m_y_pologoon_02_p", "g_m_y_salvaboss_01", "g_m_y_salvagoon_01", "g_m_y_salvagoon_02", "g_m_y_salvagoon_03", "g_m_y_salvagoon_03_p", "g_m_y_strpunk_01", "g_m_y_strpunk_02", "hc_driver"},
	{"hc_gunman", "hc_hacker", "ig_abigail", "ig_amandatownley", "ig_andreas", "ig_ashley", "ig_ballasog", "ig_bankman", "ig_barry", "ig_barry_p"},
	{"ig_bestmen", "ig_beverly", "ig_beverly_p", "ig_brad", "ig_bride", "ig_car3guy1", "ig_car3guy2", "ig_casey", "ig_chef", "ig_chengsr"},
	{"ig_chrisformage", "ig_clay", "ig_claypain", "ig_cletus", "ig_dale", "ig_davenorton", "ig_denise", "ig_devin", "ig_dom", "ig_dreyfuss"},
	{"ig_drfriedlander", "ig_fabien", "ig_fbisuit_01", "ig_floyd", "ig_groom", "ig_hao", "ig_hunter", "ig_janet", "ig_jay_norris", "ig_jewelass"},
	{"ig_jimmyboston", "ig_jimmydisanto", "ig_joeminuteman", "ig_johnnyklebitz", "ig_josef", "ig_josh", "ig_kerrymcintosh", "ig_lamardavis", "ig_lazlow", "ig_lestercrest"},
	{"ig_lifeinvad_01", "ig_lifeinvad_02", "ig_magenta", "ig_manuel", "ig_marnie", "ig_maryann", "ig_maude", "ig_michelle", "ig_milton", "ig_molly"},
	{"ig_mrk", "ig_mrsphillips", "ig_mrs_thornhill", "ig_natalia", "ig_nervousron", "ig_nigel", "ig_old_man1a", "ig_old_man2", "ig_omega", "ig_oneil"},
	{"ig_orleans", "ig_ortega", "ig_paper", "ig_patricia", "ig_priest", "ig_prolsec_02", "ig_ramp_gang", "ig_ramp_hic", "ig_ramp_hipster", "ig_ramp_mex"},
	{"ig_roccopelosi", "ig_russiandrunk", "ig_screen_writer", "ig_siemonyetarian", "ig_solomon", "ig_stevehains", "ig_stretch", "ig_talina", "ig_tanisha", "ig_taocheng"},
	{"ig_taostranslator", "ig_taostranslator_p", "ig_tenniscoach", "ig_terry", "ig_tomepsilon", "ig_tonya", "ig_tracydisanto", "ig_trafficwarden", "ig_tylerdix", "ig_wade"},
	{"ig_zimbor", "mp_f_deadhooker", "mp_f_freemode_01", "mp_f_misty_01", "mp_f_stripperlite", "mp_g_m_pros_01", "mp_headtargets", "mp_m_claude_01", "mp_m_exarmy_01", "mp_m_famdd_01"},
	{"mp_m_fibsec_01", "mp_m_freemode_01", "mp_m_marston_01", "mp_m_niko_01", "mp_m_shopkeep_01", "mp_s_m_armoured_01", "", "", "", ""},
	{"", "s_f_m_fembarber", "s_f_m_maid_01", "s_f_m_shop_high", "s_f_m_sweatshop_01", "s_f_y_airhostess_01", "s_f_y_bartender_01", "s_f_y_baywatch_01", "s_f_y_cop_01", "s_f_y_factory_01"},
	{"s_f_y_hooker_01", "s_f_y_hooker_02", "s_f_y_hooker_03", "s_f_y_migrant_01", "s_f_y_movprem_01", "s_f_y_ranger_01", "s_f_y_scrubs_01", "s_f_y_sheriff_01", "s_f_y_shop_low", "s_f_y_shop_mid"},
	{"s_f_y_stripperlite", "s_f_y_stripper_01", "s_f_y_stripper_02", "s_f_y_sweatshop_01", "s_m_m_ammucountry", "s_m_m_armoured_01", "s_m_m_armoured_02", "s_m_m_autoshop_01", "s_m_m_autoshop_02", "s_m_m_bouncer_01"},
	{"s_m_m_chemsec_01", "s_m_m_ciasec_01", "s_m_m_cntrybar_01", "s_m_m_dockwork_01", "s_m_m_doctor_01", "s_m_m_fiboffice_01", "s_m_m_fiboffice_02", "s_m_m_gaffer_01", "s_m_m_gardener_01", "s_m_m_gentransport"},
	{"s_m_m_hairdress_01", "s_m_m_highsec_01", "s_m_m_highsec_02", "s_m_m_janitor", "s_m_m_lathandy_01", "s_m_m_lifeinvad_01", "s_m_m_linecook", "s_m_m_lsmetro_01", "s_m_m_mariachi_01", "s_m_m_marine_01"},
	{"s_m_m_marine_02", "s_m_m_migrant_01", "u_m_y_zombie_01", "s_m_m_movprem_01", "s_m_m_movspace_01", "s_m_m_paramedic_01", "s_m_m_pilot_01", "s_m_m_pilot_02", "s_m_m_postal_01", "s_m_m_postal_02"},
	{"s_m_m_prisguard_01", "s_m_m_scientist_01", "s_m_m_security_01", "s_m_m_snowcop_01", "s_m_m_strperf_01", "s_m_m_strpreach_01", "s_m_m_strvend_01", "s_m_m_trucker_01", "s_m_m_ups_01", "s_m_m_ups_02"},
	{"s_m_o_busker_01", "s_m_y_airworker", "s_m_y_ammucity_01", "s_m_y_armymech_01", "s_m_y_autopsy_01", "s_m_y_barman_01", "s_m_y_baywatch_01", "s_m_y_blackops_01", "s_m_y_blackops_02", "s_m_y_busboy_01"},
	{"s_m_y_chef_01", "s_m_y_clown_01", "s_m_y_construct_01", "s_m_y_construct_02", "s_m_y_cop_01", "s_m_y_dealer_01", "s_m_y_devinsec_01", "s_m_y_dockwork_01", "s_m_y_doorman_01", "s_m_y_dwservice_01"},
	{"s_m_y_dwservice_02", "s_m_y_factory_01", "s_m_y_fireman_01", "s_m_y_garbage", "s_m_y_grip_01", "s_m_y_hwaycop_01", "s_m_y_marine_01", "s_m_y_marine_02", "s_m_y_marine_03", "s_m_y_mime"},
	{"s_m_y_pestcont_01", "s_m_y_pilot_01", "s_m_y_prismuscl_01", "s_m_y_prisoner_01", "s_m_y_ranger_01", "s_m_y_robber_01", "s_m_y_sheriff_01", "s_m_y_shop_mask", "s_m_y_strvend_01", "s_m_y_swat_01"},
	{"s_m_y_uscg_01", "s_m_y_valet_01", "s_m_y_waiter_01", "s_m_y_winclean_01", "s_m_y_xmech_01", "s_m_y_xmech_02", "u_f_m_corpse_01", "u_f_m_miranda", "u_f_m_promourn_01", "u_f_o_moviestar"},
	{"u_f_o_prolhost_01", "u_f_y_bikerchic", "u_f_y_comjane", "u_f_y_corpse_01", "u_f_y_corpse_02", "u_f_y_hotposh_01", "u_f_y_jewelass_01", "u_f_y_mistress", "u_f_y_poppymich", "u_f_y_princess"},
	{"u_f_y_spyactress", "u_m_m_aldinapoli", "u_m_m_bankman", "u_m_m_bikehire_01", "u_m_m_fibarchitect", "u_m_m_filmdirector", "u_m_m_glenstank_01", "u_m_m_griff_01", "u_m_m_jesus_01", "u_m_m_jewelsec_01"},
	{"u_m_m_jewelthief", "u_m_m_markfost", "u_m_m_partytarget", "u_m_m_prolsec_01", "u_m_m_promourn_01", "u_m_m_rivalpap", "u_m_m_spyactor", "u_m_m_willyfist", "u_m_o_finguru_01", "u_m_o_taphillbilly"},
	{"u_m_o_tramp_01", "u_m_y_abner", "u_m_y_antonb", "u_m_y_babyd", "u_m_y_baygor", "u_m_y_burgerdrug_01", "u_m_y_chip", "u_m_y_cyclist_01", "u_m_y_fibmugger_01", "u_m_y_guido_01"},
	{"u_m_y_gunvend_01", "u_m_y_hippie_01", "u_m_y_imporage", "u_m_y_justin", "u_m_y_mani", "u_m_y_militarybum", "u_m_y_paparazzi", "u_m_y_party_01", "u_m_y_pogo_01", "u_m_y_prisoner_01"}
};

LPCSTR pedModelNames[69][10] = {
	{"MICHAEL", "FRANKLIN", "TREVOR", "BOAR", "CHIMP", "COW", "COYOTE", "DEER", "FISH", "HEN"},
	{ "CAT", "HAWK", "CORMORANT", "CROW", "DOLPHIN", "HUMPBACK", "WHALE", "PIGEON", "SEAGULL", "SHARKHAMMER" },
	{"PIG", "RAT", "RHESUS", "CHOP", "HUSKY", "MTLION", "RETRIEVER", "SHARKTIGER", "SHEPHERD", "ALIEN"},
	{"BEACH", "BEVHILLS", "BEVHILLS", "BODYBUILD", "BUSINESS", "DOWNTOWN", "EASTSA", "EASTSA", "FATBLA", "FATCULT"},
	{"FATWHITE", "KTOWN", "KTOWN", "PROLHOST", "SALTON", "SKIDROW", "SOUCENTMC", "SOUCENT", "SOUCENT", "TOURIST"},
	{"TRAMPBEAC", "TRAMP", "GENSTREET", "INDIAN", "KTOWN", "SALTON", "SOUCENT", "SOUCENT", "BEACH", "BEVHILLS"},
	{"BEVHILLS", "BEVHILLS", "BEVHILLS", "BUSINESS", "BUSINESS", "BUSINESS", "BUSINESS", "EASTSA", "EASTSA", "EASTSA"},
	{"EPSILON", "FITNESS", "FITNESS", "GENHOT", "GOLFER", "HIKER", "HIPPIE", "HIPSTER", "HIPSTER", "HIPSTER"},
	{"HIPSTER", "INDIAN", "JUGGALO", "RUNNER", "RURMETH", "SCDRESSY", "SKATER", "SOUCENT", "SOUCENT", "SOUCENT"},
	{"TENNIS", "TOPLESS", "TOURIST", "TOURIST", "VINEWOOD", "VINEWOOD", "VINEWOOD", "VINEWOOD", "YOGA", "ACULT"},
	{"AFRIAMER", "BEACH", "BEACH", "BEVHILLS", "BEVHILLS", "BUSINESS", "EASTSA", "EASTSA", "FARMER", "FATLATIN"},
	{"GENFAT", "GENFAT", "GOLFER", "HASJEW", "HILLBILLY", "HILLBILLY", "INDIAN", "KTOWN", "MALIBU", "MEXCNTRY"},
	{"MEXLABOR", "OG_BOSS", "PAPARAZZI", "POLYNESIAN", "PROLHOST", "RURMETH", "SALTON", "SALTON", "SALTON", "SALTON"},
	{"SKATER", "SKIDROW", "SOCENLAT", "SOUCENT", "SOUCENT", "SOUCENT", "SOUCENT", "STLAT", "TENNIS", "TOURIST"},
	{"TRAMPBEAC", "TRAMP", "TRANVEST", "TRANVEST", "ACULT", "ACULT", "BEACH", "GENSTREET", "KTOWN", "SALTON"},
	{"SOUCENT", "SOUCENT", "SOUCENT", "TRAMP", "ACULT", "ACULT", "BEACHVESP", "BEACHVESP", "BEACH", "BEACH"},
	{"BEACH", "BEVHILLS", "BEVHILLS", "BREAKDANCE", "BUSICAS", "BUSINESS", "BUSINESS", "BUSINESS", "CYCLIST", "DHILL"},
	{"DOWNTOWN", "EASTSA", "EASTSA", "EPSILON", "EPSILON", "GAY", "GAY", "GENSTREET", "GENSTREET", "GOLFER"},
	{"HASJEW", "HIKER", "HIPPY", "HIPSTER", "HIPSTER", "HIPSTER", "INDIAN", "JETSKI", "JUGGALO", "KTOWN"},
	{"KTOWN", "LATINO", "METHHEAD", "MEXTHUG", "MOTOX", "MOTOX", "MUSCLBEAC", "MUSCLBEAC", "POLYNESIAN", "ROADCYC"},
	{"RUNNER", "RUNNER", "SALTON", "SKATER", "SKATER", "SOUCENT", "SOUCENT", "SOUCENT", "SOUCENT", "STBLA"},
	{"STBLA", "STLAT", "STWHI", "STWHI", "SUNBATHE", "SURFER", "VINDOUCHE", "VINEWOOD", "VINEWOOD", "VINEWOOD"},
	{"VINEWOOD", "YOGA", "PROLDRIVER", "RSRANGER", "SBIKE", "STAGGRM", "TATTOO", "ABIGAIL", "ANITA", "ANTON"},
	{"BALLASOG", "BRIDE", "BURGERDRUG", "CAR3GUY1", "CAR3GUY2", "CHEF", "CHIN_GOON", "CLETUS", "COP", "CUSTOMER"},
	{"DENISE_FRIEND", "FOS_REP", "G", "GROOM", "DLR", "HAO", "HUGH", "IMRAN", "JANITOR", "MAUDE"},
	{"MWEATHER", "ORTEGA", "OSCAR", "PORNDUDES", "PORNDUDES_P", "PROLOGUEDRIVER", "PROLSEC", "GANG", "HIC", "HIPSTER"},
	{"MARINE", "MEX", "REPORTER", "ROCCOPELOSI", "SCREEN_WRITER", "STRIPPER", "STRIPPER", "TONYA", "TRAFFICWARDEN", "AMANDATOWNLEY"},
	{"ANDREAS", "ASHLEY", "BANKMAN", "BARRY", "BARRY_P", "BEVERLY", "BEVERLY_P", "BRAD", "BRADCADAVER", "CARBUYER"},
	{"CASEY", "CHENGSR", "CHRISFORMAGE", "CLAY", "DALE", "DAVENORTON", "DEBRA", "DENISE", "DEVIN", "DOM"},
	{"DREYFUSS", "DRFRIEDLANDER", "FABIEN", "FBISUIT", "FLOYD", "GUADALOPE", "GURK", "HUNTER", "JANET", "JEWELASS"},
	{"JIMMYBOSTON", "JIMMYDISANTO", "JOEMINUTEMAN", "JOHNNYKLEBITZ", "JOSEF", "JOSH", "LAMARDAVIS", "LAZLOW", "LESTERCREST", "LIFEINVAD"},
	{"MAGENTA", "MANUEL", "MARNIE", "MARTINMADRAZO", "MARYANN", "MICHELLE", "MILTON", "MOLLY", "MOVPREMF", "MOVPREMMALE"},
	{"MRK", "MRSPHILLIPS", "MRS_THORNHILL", "NATALIA", "NERVOUSRON", "NIGEL", "OLD_MAN1A", "OLD_MAN2", "OMEGA", "ORLEANS"},
	{"PAPER", "PAPER_P", "PATRICIA", "PRIEST", "PROLSEC", "RUSSIANDRUNK", "SIEMONYETARIAN", "SOLOMON", "STEVEHAINS", "STRETCH"},
	{"TANISHA", "TAOCHENG", "TAOSTRANSLATOR", "TENNISCOACH", "TERRY", "TOM", "TOMEPSILON", "TRACYDISANTO", "WADE", "ZIMBOR"},
	{"BALLAS", "FAMILIES", "LOST", "VAGOS", "ARMBOSS", "ARMGOON", "ARMLIEUT", "CHEMWORK", "CHEMWORK_P", "CHIBOSS"},
	{"CHIBOSS_P", "CHICOLD", "CHICOLD_P", "CHIGOON", "CHIGOON_P", "CHIGOON", "KORBOSS", "MEXBOSS", "MEXBOSS", "ARMGOON"},
	{"AZTECA", "BALLAEAST", "BALLAORIG", "BALLASOUT", "FAMCA", "FAMDNF", "FAMFOR", "KOREAN", "KOREAN", "KORLIEUT"},
	{"LOST", "LOST", "LOST", "MEXGANG", "MEXGOON", "MEXGOON", "MEXGOON", "MEXGOON_P", "POLOGOON", "POLOGOON_P"},
	{"POLOGOON", "POLOGOON_P", "SALVABOSS", "SALVAGOON", "SALVAGOON", "SALVAGOON", "SALVAGOON_P", "STRPUNK", "STRPUNK", "HC_DRIVER"},
	{"HC_GUNMAN", "HC_HACKER", "ABIGAIL", "AMANDATOWNLEY", "ANDREAS", "ASHLEY", "BALLASOG", "BANKMAN", "BARRY", "BARRY_P"},
	{"BESTMEN", "BEVERLY", "BEVERLY_P", "BRAD", "BRIDE", "CAR3GUY1", "CAR3GUY2", "CASEY", "CHEF", "CHENGSR"},
	{"CHRISFORMAGE", "CLAY", "CLAYPAIN", "CLETUS", "DALE", "DAVENORTON", "DENISE", "DEVIN", "DOM", "DREYFUSS"},
	{"DRFRIEDLANDER", "FABIEN", "FBISUIT", "FLOYD", "GROOM", "HAO", "HUNTER", "JANET", "JAY_NORRIS", "JEWELASS"},
	{"JIMMYBOSTON", "JIMMYDISANTO", "JOEMINUTEMAN", "JOHNNYKLEBITZ", "JOSEF", "JOSH", "KERRYMCINTOSH", "LAMARDAVIS", "LAZLOW", "LESTERCREST"},
	{"LIFEINVAD", "LIFEINVAD", "MAGENTA", "MANUEL", "MARNIE", "MARYANN", "MAUDE", "MICHELLE", "MILTON", "MOLLY"},
	{"MRK", "MRSPHILLIPS", "MRS_THORNHILL", "NATALIA", "NERVOUSRON", "NIGEL", "OLD_MAN1A", "OLD_MAN2", "OMEGA", "ONEIL"},
	{"ORLEANS", "ORTEGA", "PAPER", "PATRICIA", "PRIEST", "PROLSEC", "GANG", "HIC", "HIPSTER", "MEX"},
	{"ROCCOPELOSI", "RUSSIANDRUNK", "SCREEN_WRITER", "SIEMONYETARIAN", "SOLOMON", "STEVEHAINS", "STRETCH", "TALINA", "TANISHA", "TAOCHENG"},
	{"TAOSTRANSLATOR", "TAOSTRANSLATOR_P", "TENNISCOACH", "TERRY", "TOMEPSILON", "TONYA", "TRACYDISANTO", "TRAFFICWARDEN", "TYLERDIX", "WADE"},
	{"ZIMBOR", "DEADHOOKER", "FREEMODE", "MISTY", "STRIPPERLITE", "PROS", "MP_HEADTARGETS", "CLAUDE", "EXARMY", "FAMDD"},
	{"FIBSEC", "FREEMODE", "MARSTON", "NIKO", "SHOPKEEP", "ARMOURED", "NONE", "NONE", "NONE", "NONE"},
	{"NONE", "FEMBARBER", "MAID", "SHOP_HIGH", "SWEATSHOP", "AIRHOSTESS", "BARTENDER", "BAYWATCH", "COP", "FACTORY"},
	{"HOOKER", "HOOKER", "HOOKER", "MIGRANT", "MOVPREM", "RANGER", "SCRUBS", "SHERIFF", "SHOP_LOW", "SHOP_MID"},
	{"STRIPPERLITE", "STRIPPER", "STRIPPER", "SWEATSHOP", "AMMUCOUNTRY", "ARMOURED", "ARMOURED", "AUTOSHOP", "AUTOSHOP", "BOUNCER"},
	{"CHEMSEC", "CIASEC", "CNTRYBAR", "DOCKWORK", "DOCTOR", "FIBOFFICE", "FIBOFFICE", "GAFFER", "GARDENER", "GENTRANSPORT"},
	{"HAIRDRESS", "HIGHSEC", "HIGHSEC", "JANITOR", "LATHANDY", "LIFEINVAD", "LINECOOK", "LSMETRO", "MARIACHI", "MARINE"},
	{"MARINE", "MIGRANT", "ZOMBIE", "MOVPREM", "MOVSPACE", "PARAMEDIC", "PILOT", "PILOT", "POSTAL", "POSTAL"},
	{"PRISGUARD", "SCIENTIST", "SECURITY", "SNOWCOP", "STRPERF", "STRPREACH", "STRVEND", "TRUCKER", "UPS", "UPS"},
	{"BUSKER", "AIRWORKER", "AMMUCITY", "ARMYMECH", "AUTOPSY", "BARMAN", "BAYWATCH", "BLACKOPS", "BLACKOPS", "BUSBOY"},
	{"CHEF", "CLOWN", "CONSTRUCT", "CONSTRUCT", "COP", "DEALER", "DEVINSEC", "DOCKWORK", "DOORMAN", "DWSERVICE"},
	{"DWSERVICE", "FACTORY", "FIREMAN", "GARBAGE", "GRIP", "HWAYCOP", "MARINE", "MARINE", "MARINE", "MIME"},
	{"PESTCONT", "PILOT", "PRISMUSCL", "PRISONER", "RANGER", "ROBBER", "SHERIFF", "SHOP_MASK", "STRVEND", "SWAT"},
	{"USCG", "VALET", "WAITER", "WINCLEAN", "XMECH", "XMECH", "CORPSE", "MIRANDA", "PROMOURN", "MOVIESTAR"},
	{"PROLHOST", "BIKERCHIC", "COMJANE", "CORPSE", "CORPSE", "HOTPOSH", "JEWELASS", "MISTRESS", "POPPYMICH", "PRINCESS"},
	{"SPYACTRESS", "ALDINAPOLI", "BANKMAN", "BIKEHIRE", "FIBARCHITECT", "FILMDIRECTOR", "GLENSTANK", "GRIFF", "JESUS", "JEWELSEC"},
	{"JEWELTHIEF", "MARKFOST", "PARTYTARGET", "PROLSEC", "PROMOURN", "RIVALPAP", "SPYACTOR", "WILLYFIST", "FINGURU", "TAPHILLBILLY"},
	{"TRAMP", "ABNER", "ANTONB", "BABYD", "BAYGOR", "BURGERDRUG", "CHIP", "CYCLIST", "FIBMUGGER", "GUIDO"},
	{"GUNVEND", "HIPPIE", "IMPORAGE", "JUSTIN", "MANI", "MILITARYBUM", "PAPARAZZI", "PARTY", "POGO", "PRISONER"}
};

int skinchangerActiveLineIndex = 0;
int skinchangerActiveItemIndex = 0;

bool process_skinchanger_menu()
{
	DWORD waitTime = 150;
	const int lineCount = 69;
	const int itemCount = 10;
	const int itemCountLastLine = itemCount;
	while (true)
	{
		// timed menu draw, used for pause after active line switch
		DWORD maxTickCount = GetTickCount() + waitTime;
		do 
		{
			// draw menu
			char caption[32];
			sprintf_s(caption, "SKIN CHANGER   %d / %d", skinchangerActiveLineIndex + 1, lineCount);
			draw_menu_line(caption, 350.0, 15.0, 18.0, 0.0, 5.0, false, true);
			for (int i = 0; i < itemCount; i++)
				if (strlen(pedModels[skinchangerActiveLineIndex][i]) || strcmp(pedModelNames[skinchangerActiveLineIndex][i], "NONE") == 0)
					draw_menu_line(pedModelNames[skinchangerActiveLineIndex][i], 100.0f, 5.0f, 200.0f, 100.0f + i * 110.0f, 5.0f, i == skinchangerActiveItemIndex, false, false);
			
			update_features();
			WAIT(0);
		} while (GetTickCount() < maxTickCount);
		waitTime = 0;

		bool bSelect, bBack, bUp, bDown, bLeft, bRight;
		get_button_state(&bSelect, &bBack, &bUp, &bDown, &bLeft, &bRight);
		
		if (bSelect)
		{
			menu_beep();
			DWORD model = GAMEPLAY::GET_HASH_KEY((char *)pedModels[skinchangerActiveLineIndex][skinchangerActiveItemIndex]);
			if (STREAMING::IS_MODEL_IN_CDIMAGE(model) && STREAMING::IS_MODEL_VALID(model))
			{
				STREAMING::REQUEST_MODEL(model);				
				while (!STREAMING::HAS_MODEL_LOADED(model))	WAIT(0); 
				//STREAMING::LOAD_ALL_OBJECTS_NOW();
				PLAYER::SET_PLAYER_MODEL(PLAYER::PLAYER_ID(), model);
				//PED::SET_PED_RANDOM_COMPONENT_VARIATION(PLAYER::PLAYER_PED_ID(), FALSE);
				PED::SET_PED_DEFAULT_COMPONENT_VARIATION(PLAYER::PLAYER_PED_ID());				
				WAIT(0);
				for (int i = 0; i < 12; i++)
					for (int j = 0; j < 100; j++)
					{
						int drawable = rand() % 10;
						int texture = rand() % 10;
						if (PED::IS_PED_COMPONENT_VARIATION_VALID(PLAYER::PLAYER_PED_ID(), i, drawable, texture))
						{
							PED::SET_PED_COMPONENT_VARIATION(PLAYER::PLAYER_PED_ID(), i, drawable, texture, 0);
							break;
						}
					}
				skinchanger_used = true;
				WAIT(100);
				STREAMING::SET_MODEL_AS_NO_LONGER_NEEDED(model);				
				waitTime = 200;
			}
		} else
		if (bBack)
		{
			menu_beep();
			break;
		} else
		if (bRight)
		{
			menu_beep();
			skinchangerActiveItemIndex++;
			int itemsMax = (skinchangerActiveLineIndex == (lineCount - 1)) ? itemCountLastLine : itemCount;
			if (skinchangerActiveItemIndex == itemsMax) 
				skinchangerActiveItemIndex = 0;			
			waitTime = 100;
		} else
		if (bLeft)
		{
			menu_beep();
			if (skinchangerActiveItemIndex == 0) 
				skinchangerActiveItemIndex = (skinchangerActiveLineIndex == (lineCount - 1)) ? itemCountLastLine : itemCount;
			skinchangerActiveItemIndex--;
			waitTime = 100;
		} else
		if (bUp)
		{
			menu_beep();
			if (skinchangerActiveLineIndex == 0) 
				skinchangerActiveLineIndex = lineCount;
			skinchangerActiveLineIndex--;
			waitTime = 200;
		} else
		if (bDown)
		{
			menu_beep();
			skinchangerActiveLineIndex++;
			if (skinchangerActiveLineIndex == lineCount) 
				skinchangerActiveLineIndex = 0;			
			waitTime = 200;
		}
		if (skinchangerActiveLineIndex == (lineCount - 1))
			if (skinchangerActiveItemIndex >= itemCountLastLine)
				skinchangerActiveItemIndex = 0;
	}
	return false;
}

int teleportActiveLineIndex = 0;

bool process_teleport_menu()
{
	const float lineWidth = 250.0;
	const int lineCount	= 17;

	std::string caption = "TELEPORT";

	static struct {
		LPCSTR  text;
		float x;
		float y;
		float z;
	} lines[lineCount] = {
			{ "MARKER" },
			{ "MICHAEL'S HOUSE", -852.4f, 160.0f, 65.6f },
			{ "FRANKLIN'S HOUSE", 7.9f, 548.1f, 175.5f },
			{ "TREVOR'S TRAILER", 1985.7f, 3812.2f, 32.2f },
			{ "AIRPORT ENTRANCE", -1034.6f, -2733.6f, 13.8f },
			{ "AIRPORT FIELD", -1336.0f, -3044.0f, 13.9f },
			{ "ELYSIAN ISLAND", 338.2f, -2715.9f, 38.5f },
			{ "JETSAM", 760.4f, -2943.2f, 5.8f },
			{ "STRIPCLUB", 127.4f, -1307.7f, 29.2f },
			{ "ELBURRO HEIGHTS", 1384.0f, -2057.1f, 52.0f },
			{ "FERRIS WHEEL", -1670.7f, -1125.0f, 13.0f },
			{ "CHUMASH", -3192.6f, 1100.0f, 20.2f },
			{ "WINDFARM", 2354.0f, 1830.3f, 101.1f },
			{ "MILITARY BASE", -2047.4f, 3132.1f, 32.8f },
			{ "MCKENZIE AIRFIELD", 2121.7f, 4796.3f, 41.1f },
			{ "DESERT AIRFIELD", 1747.0f, 3273.7f, 41.1f },
			{ "CHILLIAD", 425.4f, 5614.3f, 766.5f }
	};

	DWORD waitTime = 150;
	while (true)
	{
		// timed menu draw, used for pause after active line switch
		DWORD maxTickCount = GetTickCount() + waitTime;
		do 
		{
			// draw menu
			draw_menu_line(caption, lineWidth, 15.0, 18.0, 0.0, 5.0, false, true);
			for (int i = 0; i < lineCount; i++)
				if (i != teleportActiveLineIndex)
					draw_menu_line(lines[i].text, lineWidth, 9.0, 60.0 + i * 36.0, 0.0, 9.0, false, false);
			draw_menu_line(lines[teleportActiveLineIndex].text, lineWidth + 1.0, 11.0, 56.0 + teleportActiveLineIndex * 36.0, 0.0, 7.0, true, false);

			update_features();
			WAIT(0);
		} while (GetTickCount() < maxTickCount);
		waitTime = 0;

		// process buttons
		bool bSelect, bBack, bUp, bDown;
		get_button_state(&bSelect, &bBack, &bUp, &bDown, NULL, NULL);
		if (bSelect)
		{
			menu_beep();

			// get entity to teleport
			Entity e = PLAYER::PLAYER_PED_ID();
			if (PED::IS_PED_IN_ANY_VEHICLE(e, 0)) 
				e = PED::GET_VEHICLE_PED_IS_USING(e);

			// get coords
			Vector3 coords;
			bool success = false;
			if (teleportActiveLineIndex == 0) // marker
			{			
				bool blipFound = false;
				// search for marker blip
				int blipIterator = UI::_GET_BLIP_INFO_ID_ITERATOR();
				for (Blip i = UI::GET_FIRST_BLIP_INFO_ID(blipIterator); UI::DOES_BLIP_EXIST(i) != 0; i = UI::GET_NEXT_BLIP_INFO_ID(blipIterator))
				{
					if (UI::GET_BLIP_INFO_ID_TYPE(i) == 4) 
					{
						coords = UI::GET_BLIP_INFO_ID_COORD(i);
						blipFound = true;
						break;
					}
				}	
				if (blipFound)
				{
					// load needed map region and check height levels for ground existence
					bool groundFound = false;
					static float groundCheckHeight[] = {
						100.0, 150.0, 50.0, 0.0, 200.0, 250.0, 300.0, 350.0, 400.0, 
						450.0, 500.0, 550.0, 600.0, 650.0, 700.0, 750.0, 800.0
					};					
					for (int i = 0; i < sizeof(groundCheckHeight) / sizeof(float); i++)
					{
						ENTITY::SET_ENTITY_COORDS_NO_OFFSET(e, coords.x, coords.y, groundCheckHeight[i], 0, 0, 1);
						WAIT(100);
						if (GAMEPLAY::GET_GROUND_Z_FOR_3D_COORD(coords.x, coords.y, groundCheckHeight[i], &coords.z))
						{
							groundFound = true;
							coords.z += 3.0;
							break;
						}
					}
					// if ground not found then set Z in air and give player a parachute
					if (!groundFound)
					{
						coords.z = 1000.0;
						WEAPON::GIVE_DELAYED_WEAPON_TO_PED(PLAYER::PLAYER_PED_ID(), 0xFBAB5776, 1, 0);
					}
					success = true;
				} else
				{
					set_status_text("map marker isn't set");
				}

			} else // predefined coords
			{
				coords.x = lines[teleportActiveLineIndex].x;
				coords.y = lines[teleportActiveLineIndex].y;
				coords.z = lines[teleportActiveLineIndex].z;
				success = true;
			}

			// set player pos
			if (success)
			{
				ENTITY::SET_ENTITY_COORDS_NO_OFFSET(e, coords.x, coords.y, coords.z, 0, 0, 1);
				WAIT(0);
				set_status_text("teleported");
				return true;
			}
			
			waitTime = 200;
		} else
		if (bBack || trainer_switch_pressed())
		{
			menu_beep();
			break;
		} else
		if (bUp)
		{
			menu_beep();
			if (teleportActiveLineIndex == 0) 
				teleportActiveLineIndex = lineCount;
			teleportActiveLineIndex--;
			waitTime = 150;
		} else
		if (bDown)
		{
			menu_beep();
			teleportActiveLineIndex++;
			if (teleportActiveLineIndex == lineCount) 
				teleportActiveLineIndex = 0;			
			waitTime = 150;
		}
	}
	return false;
}

std::string line_as_str(std::string text, bool *pState)
{
	while (text.size() < 18) text += " ";
	return text + (pState ? (*pState ? " [ON]" : " [OFF]") : "");
}

int activeLineIndexPlayer = 0;

void process_player_menu()
{
	const float lineWidth = 250.0;
	const int lineCount = 15;
	
	std::string caption = "PLAYER  OPTIONS";

	static struct {
		LPCSTR		text;
		bool		*pState;
		bool		*pUpdated;
	} lines[lineCount] = {
		{"SKIN CHANGER", NULL, NULL},
		{"TELEPORT", NULL, NULL},
		{"FIX PLAYER", NULL, NULL},
		{"RESET SKIN", NULL, NULL},
		{"ADD CASH", NULL, NULL},
		{"WANTED UP", NULL, NULL},
		{"WANTED DOWN", NULL, NULL},
		{"NEVER WANTED", &featurePlayerNeverWanted, NULL},
		{"INVINCIBLE", &featurePlayerInvincible, &featurePlayerInvincibleUpdated},
		{"POLICE IGNORED", &featurePlayerIgnored, &featurePlayerIgnoredUpdated},
		{"UNLIM ABILITY", &featurePlayerUnlimitedAbility, NULL},
		{"NOISELESS", &featurePlayerNoNoise, &featurePlayerNoNoiseUpdated},
		{"FAST SWIM", &featurePlayerFastSwim, &featurePlayerFastSwimUpdated},
		{"FAST RUN", &featurePlayerFastRun, &featurePlayerFastRunUpdated},
		{"SUPER JUMP", &featurePlayerSuperJump, NULL}
	};

	DWORD waitTime = 150;
	while (true)
	{
		// timed menu draw, used for pause after active line switch
		DWORD maxTickCount = GetTickCount() + waitTime;
		do 
		{
			// draw menu
			draw_menu_line(caption, lineWidth, 15.0, 18.0, 0.0, 5.0, false, true);
			for (int i = 0; i < lineCount; i++)
				if (i != activeLineIndexPlayer)
					draw_menu_line(line_as_str(lines[i].text, lines[i].pState), 
								   lineWidth, 9.0, 60.0 + i * 36.0, 0.0, 9.0, false, false);
			draw_menu_line(line_as_str(lines[activeLineIndexPlayer].text, lines[activeLineIndexPlayer].pState), 
						   lineWidth + 1.0, 11.0, 56.0 + activeLineIndexPlayer * 36.0, 0.0, 7.0, true, false);

			update_features();
			WAIT(0);
		} while (GetTickCount() < maxTickCount);
		waitTime = 0;

		// process buttons
		bool bSelect, bBack, bUp, bDown;
		get_button_state(&bSelect, &bBack, &bUp, &bDown, NULL, NULL);
		if (bSelect)
		{
			menu_beep();

			// common variables
			BOOL bPlayerExists = ENTITY::DOES_ENTITY_EXIST(PLAYER::PLAYER_PED_ID());
			Player player = PLAYER::PLAYER_ID();
			Ped playerPed = PLAYER::PLAYER_PED_ID();

			switch (activeLineIndexPlayer)
			{
				// skin changer
				case 0:
					if (process_skinchanger_menu())	return;
					break;
				// teleport
				case 1:
					if (process_teleport_menu()) return;
					break;
				// fix player
				case 2:
					{
						ENTITY::SET_ENTITY_HEALTH(playerPed, ENTITY::GET_ENTITY_MAX_HEALTH(playerPed));
						PED::ADD_ARMOUR_TO_PED(playerPed, PLAYER::GET_PLAYER_MAX_ARMOUR(player) - PED::GET_PED_ARMOUR(playerPed));
						if (PED::IS_PED_IN_ANY_VEHICLE(playerPed, 0))
						{
							Vehicle playerVeh = PED::GET_VEHICLE_PED_IS_USING(playerPed);
							if (ENTITY::DOES_ENTITY_EXIST(playerVeh) && !ENTITY::IS_ENTITY_DEAD(playerVeh))
								VEHICLE::SET_VEHICLE_FIXED(playerVeh);
						}
						set_status_text("player fixed");
					}
					break;
				// reset model skin
				case 3:
					{
						PED::SET_PED_DEFAULT_COMPONENT_VARIATION(playerPed);
						set_status_text("using default model skin");
					}
					break;
				// add cash
				case 4: 
					for (int i = 0; i < 3; i++)
					{
						char statNameFull[32];
						sprintf_s(statNameFull, "SP%d_TOTAL_CASH", i);
						Hash hash = GAMEPLAY::GET_HASH_KEY(statNameFull);
						int val;
						STATS::STAT_GET_INT(hash, &val, -1);
						val += 1000000;
						STATS::STAT_SET_INT(hash, val, 1);
					}
					set_status_text("cash added");
					break;
				// wanted up
				case 5:	
					if (bPlayerExists && PLAYER::GET_PLAYER_WANTED_LEVEL(player) < 5)
					{
						PLAYER::SET_PLAYER_WANTED_LEVEL(player, PLAYER::GET_PLAYER_WANTED_LEVEL(player) + 1, 0);
						PLAYER::SET_PLAYER_WANTED_LEVEL_NOW(player, 0);
						set_status_text("wanted up");
					}
					break;
				// wanted down
				case 6:	
					if (bPlayerExists && PLAYER::GET_PLAYER_WANTED_LEVEL(player) > 0)
					{
						PLAYER::SET_PLAYER_WANTED_LEVEL(player, PLAYER::GET_PLAYER_WANTED_LEVEL(player) - 1, 0);
						PLAYER::SET_PLAYER_WANTED_LEVEL_NOW(player, 0);
						set_status_text("wanted down");
					}
					break;
				// switchable features
				default:
					if (lines[activeLineIndexPlayer].pState)
						*lines[activeLineIndexPlayer].pState = !(*lines[activeLineIndexPlayer].pState);
					if (lines[activeLineIndexPlayer].pUpdated)
						*lines[activeLineIndexPlayer].pUpdated = true;					
			}
			waitTime = 200;
		} else
		if (bBack || trainer_switch_pressed())
		{
			menu_beep();
			break;
		} else
		if (bUp)
		{
			menu_beep();
			if (activeLineIndexPlayer == 0) 
				activeLineIndexPlayer = lineCount;
			activeLineIndexPlayer--;
			waitTime = 150;
		} else
		if (bDown)
		{
			menu_beep();
			activeLineIndexPlayer++;
			if (activeLineIndexPlayer == lineCount) 
				activeLineIndexPlayer = 0;			
			waitTime = 150;
		}
	}
}

int activeLineIndexWeapon = 0;

void process_weapon_menu()
{
	const float lineWidth = 250.0;
	const int lineCount = 6;

	std::string caption = "WEAPON  OPTIONS";

	static struct {
		LPCSTR		text;
		bool		*pState;
		bool		*pUpdated;
	} lines[lineCount] = {
		{"GET ALL WEAPON",	NULL,						  NULL},
		{"NO RELOAD",		&featureWeaponNoReload,		  NULL},
		{"FIRE AMMO",		&featureWeaponFireAmmo,		  NULL},
		{"EXPLOSIVE AMMO",  &featureWeaponExplosiveAmmo,  NULL},
		{"EXPLOSIVE MELEE", &featureWeaponExplosiveMelee, NULL},
		{"VEHICLE ROCKETS", &featureWeaponVehRockets,	  NULL}
	};

	static LPCSTR weaponNames[] = {
		"WEAPON_KNIFE", "WEAPON_NIGHTSTICK", "WEAPON_HAMMER", "WEAPON_BAT", "WEAPON_GOLFCLUB", "WEAPON_CROWBAR", 
		"WEAPON_PISTOL", "WEAPON_COMBATPISTOL", "WEAPON_APPISTOL", "WEAPON_PISTOL50", "WEAPON_MICROSMG", "WEAPON_SMG", 
		"WEAPON_ASSAULTSMG", "WEAPON_ASSAULTRIFLE", "WEAPON_CARBINERIFLE", "WEAPON_ADVANCEDRIFLE", "WEAPON_MG",
		"WEAPON_COMBATMG", "WEAPON_PUMPSHOTGUN", "WEAPON_SAWNOFFSHOTGUN", "WEAPON_ASSAULTSHOTGUN", "WEAPON_BULLPUPSHOTGUN",
		"WEAPON_STUNGUN", "WEAPON_SNIPERRIFLE", "WEAPON_HEAVYSNIPER", "WEAPON_GRENADELAUNCHER", "WEAPON_GRENADELAUNCHER_SMOKE",
		"WEAPON_RPG", "WEAPON_MINIGUN", "WEAPON_GRENADE", "WEAPON_STICKYBOMB", "WEAPON_SMOKEGRENADE", "WEAPON_BZGAS",
		"WEAPON_MOLOTOV", "WEAPON_FIREEXTINGUISHER", "WEAPON_PETROLCAN",
		"WEAPON_SNSPISTOL", "WEAPON_SPECIALCARBINE", "WEAPON_HEAVYPISTOL", "WEAPON_BULLPUPRIFLE", "WEAPON_HOMINGLAUNCHER",
		"WEAPON_PROXMINE", "WEAPON_SNOWBALL", "WEAPON_VINTAGEPISTOL", "WEAPON_DAGGER", "WEAPON_FIREWORK", "WEAPON_MUSKET",
		"WEAPON_MARKSMANRIFLE", "WEAPON_HEAVYSHOTGUN", "WEAPON_GUSENBERG", "WEAPON_HATCHET", "WEAPON_RAILGUN",
		"WEAPON_COMBATPDW"
	};

	DWORD waitTime = 150;
	while (true)
	{
		// timed menu draw, used for pause after active line switch
		DWORD maxTickCount = GetTickCount() + waitTime;
		do 
		{
			// draw menu
			draw_menu_line(caption, lineWidth, 15.0, 18.0, 0.0, 5.0, false, true);
			for (int i = 0; i < lineCount; i++)
				if (i != activeLineIndexWeapon)
					draw_menu_line(line_as_str(lines[i].text, lines[i].pState), 
								   lineWidth, 9.0, 60.0 + i * 36.0, 0.0, 9.0, false, false);
			draw_menu_line(line_as_str(lines[activeLineIndexWeapon].text, lines[activeLineIndexWeapon].pState), 
						   lineWidth + 1.0, 11.0, 56.0 + activeLineIndexWeapon * 36.0, 0.0, 7.0, true, false);

			update_features();
			WAIT(0);
		} while (GetTickCount() < maxTickCount);
		waitTime = 0;

		// process buttons
		bool bSelect, bBack, bUp, bDown;
		get_button_state(&bSelect, &bBack, &bUp, &bDown, NULL, NULL);
		if (bSelect)
		{
			menu_beep();

			// common variables
			BOOL bPlayerExists = ENTITY::DOES_ENTITY_EXIST(PLAYER::PLAYER_PED_ID());
			Player player = PLAYER::PLAYER_ID();
			Ped playerPed = PLAYER::PLAYER_PED_ID();

			switch (activeLineIndexWeapon)
			{
				case 0:
					for (int i = 0; i < sizeof(weaponNames) / sizeof(weaponNames[0]); i++)
						WEAPON::GIVE_DELAYED_WEAPON_TO_PED(playerPed, GAMEPLAY::GET_HASH_KEY((char *)weaponNames[i]), 1000, 0);
					set_status_text("all weapon added");
					break;
				// switchable features
				default:
					if (lines[activeLineIndexWeapon].pState)
						*lines[activeLineIndexWeapon].pState = !(*lines[activeLineIndexWeapon].pState);
					if (lines[activeLineIndexWeapon].pUpdated)
						*lines[activeLineIndexWeapon].pUpdated = true;					
			}
			waitTime = 200;
		} else
		if (bBack || trainer_switch_pressed())
		{
			menu_beep();
			break;
		} else
		if (bUp)
		{
			menu_beep();
			if (activeLineIndexWeapon == 0) 
				activeLineIndexWeapon = lineCount;
			activeLineIndexWeapon--;
			waitTime = 150;
		} else
		if (bDown)
		{
			menu_beep();
			activeLineIndexWeapon++;
			if (activeLineIndexWeapon == lineCount) 
				activeLineIndexWeapon = 0;			
			waitTime = 150;
		}
	}
}

LPCSTR vehicleModels[36][10] = {
	{"NINEF", "NINEF2", "BLISTA", "ASEA", "ASEA2", "BOATTRAILER", "BUS", "ARMYTANKER", "ARMYTRAILER", "ARMYTRAILER2"},
	{"SUNTRAP", "COACH", "AIRBUS", "ASTEROPE", "AIRTUG", "AMBULANCE", "BARRACKS", "BARRACKS2", "BALLER", "BALLER2"},
	{"BJXL", "BANSHEE", "BENSON", "BFINJECTION", "BIFF", "BLAZER", "BLAZER2", "BLAZER3", "BISON", "BISON2"},
	{"BISON3", "BOXVILLE", "BOXVILLE2", "BOXVILLE3", "BOBCATXL", "BODHI2", "BUCCANEER", "BUFFALO", "BUFFALO2", "BULLDOZER"},
	{"BULLET", "BLIMP", "BURRITO", "BURRITO2", "BURRITO3", "BURRITO4", "BURRITO5", "CAVALCADE", "CAVALCADE2", "POLICET"},
	{"GBURRITO", "CABLECAR", "CADDY", "CADDY2", "CAMPER", "CARBONIZZARE", "CHEETAH", "COMET2", "COGCABRIO", "COQUETTE"},
	{"CUTTER", "GRESLEY", "DILETTANTE", "DILETTANTE2", "DUNE", "DUNE2", "HOTKNIFE", "DLOADER", "DUBSTA", "DUBSTA2"},
	{"DUMP", "RUBBLE", "DOCKTUG", "DOMINATOR", "EMPEROR", "EMPEROR2", "EMPEROR3", "ENTITYXF", "EXEMPLAR", "ELEGY2"},
	{"F620", "FBI", "FBI2", "FELON", "FELON2", "FELTZER2", "FIRETRUK", "FLATBED", "FORKLIFT", "FQ2"},
	{"FUSILADE", "FUGITIVE", "FUTO", "GRANGER", "GAUNTLET", "HABANERO", "HAULER", "HANDLER", "INFERNUS", "INGOT"},
	{"INTRUDER", "ISSI2", "JACKAL", "JOURNEY", "JB700", "KHAMELION", "LANDSTALKER", "LGUARD", "MANANA", "MESA"},
	{"MESA2", "MESA3", "CRUSADER", "MINIVAN", "MIXER", "MIXER2", "MONROE", "MOWER", "MULE", "MULE2"},
	{"ORACLE", "ORACLE2", "PACKER", "PATRIOT", "PBUS", "PENUMBRA", "PEYOTE", "PHANTOM", "PHOENIX", "PICADOR"},
	{"POUNDER", "POLICE", "POLICE4", "POLICE2", "POLICE3", "POLICEOLD1", "POLICEOLD2", "PONY", "PONY2", "PRAIRIE"},
	{"PRANGER", "PREMIER", "PRIMO", "PROPTRAILER", "RANCHERXL", "RANCHERXL2", "RAPIDGT", "RAPIDGT2", "RADI", "RATLOADER"},
	{"REBEL", "REGINA", "REBEL2", "RENTALBUS", "RUINER", "RUMPO", "RUMPO2", "RHINO", "RIOT", "RIPLEY"},
	{"ROCOTO", "ROMERO", "SABREGT", "SADLER", "SADLER2", "SANDKING", "SANDKING2", "SCHAFTER2", "SCHWARZER", "SCRAP"},
	{"SEMINOLE", "SENTINEL", "SENTINEL2", "ZION", "ZION2", "SERRANO", "SHERIFF", "SHERIFF2", "SPEEDO", "SPEEDO2"},
	{"STANIER", "STINGER", "STINGERGT", "STOCKADE", "STOCKADE3", "STRATUM", "SULTAN", "SUPERD", "SURANO", "SURFER"},
	{"SURFER2", "SURGE", "TACO", "TAILGATER", "TAXI", "TRASH", "TRACTOR", "TRACTOR2", "TRACTOR3", "GRAINTRAILER"},
	{"BALETRAILER", "TIPTRUCK", "TIPTRUCK2", "TORNADO", "TORNADO2", "TORNADO3", "TORNADO4", "TOURBUS", "TOWTRUCK", "TOWTRUCK2"},
	{"UTILLITRUCK", "UTILLITRUCK2", "UTILLITRUCK3", "VOODOO2", "WASHINGTON", "STRETCH", "YOUGA", "ZTYPE", "SANCHEZ", "SANCHEZ2"},
	{"SCORCHER", "TRIBIKE", "TRIBIKE2", "TRIBIKE3", "FIXTER", "CRUISER", "BMX", "POLICEB", "AKUMA", "CARBONRS"},
	{"BAGGER", "BATI", "BATI2", "RUFFIAN", "DAEMON", "DOUBLE", "PCJ", "VADER", "VIGERO", "FAGGIO2"},
	{"HEXER", "ANNIHILATOR", "BUZZARD", "BUZZARD2", "CARGOBOB", "CARGOBOB2", "CARGOBOB3", "SKYLIFT", "POLMAV", "MAVERICK"},
	{"NEMESIS", "FROGGER", "FROGGER2", "CUBAN800", "DUSTER", "STUNT", "MAMMATUS", "JET", "SHAMAL", "LUXOR"},
	{"TITAN", "LAZER", "CARGOPLANE", "SQUALO", "MARQUIS", "DINGHY", "DINGHY2", "JETMAX", "PREDATOR", "TROPIC"},
	{"SEASHARK", "SEASHARK2", "SUBMERSIBLE", "TRAILERS", "TRAILERS2", "TRAILERS3", "TVTRAILER", "RAKETRAILER", "TANKER", "TRAILERLOGS"},
	{"TR2", "TR3", "TR4", "TRFLAT", "TRAILERSMALL", "VELUM", "ADDER", "VOLTIC", "VACCA", "BIFTA"},
	{ "SPEEDER", "PARADISE", "KALAHARI", "JESTER", "TURISMOR", "VESTRA", "ALPHA", "HUNTLEY", "THRUST", "MASSACRO" },
	{ "MASSACRO2", "ZENTORNO", "BLADE", "GLENDALE", "PANTO", "PIGALLE", "WARRENER", "RHAPSODY", "DUBSTA3", "MONSTER" },
	{ "SOVEREIGN", "INNOVATION", "HAKUCHOU", "FUROREGT", "MILJET", "COQUETTE2", "BTYPE", "BUFFALO3", "DOMINATOR2", "GAUNTLET2" },
	{ "MARSHALL", "DUKES", "DUKES2", "STALION", "STALION2", "BLISTA2", "BLISTA3", "DODO", "SUBMERSIBLE2", "HYDRA" },
	{ "INSURGENT", "INSURGENT2", "TECHNICAL", "SAVAGE", "VALKYRIE", "KURUMA", "KURUMA2", "JESTER2", "CASCO", "VELUM2" },
	{ "GUARDIAN", "ENDURO", "LECTRO", "SLAMVAN", "SLAMVAN2", "RATLOADER2", "SWIFT2", "LUXOR2", "FELTZER3", "OSIRIS" },
	{ "VIRGO", "WINDSOR", "BESRA", "SWIFT", "BLIMP2", "", "", "", "", "" }
};

int carspawnActiveLineIndex = 0;
int carspawnActiveItemIndex = 0;

bool process_carspawn_menu()
{
	DWORD waitTime = 150;
	const int lineCount = 36;
	const int itemCount = 10;
	const int itemCountLastLine = 5;
	while (true)
	{
		// timed menu draw, used for pause after active line switch
		DWORD maxTickCount = GetTickCount() + waitTime;
		do 
		{
			// draw menu
			char caption[32];
			sprintf_s(caption, "CAR SPAWNER   %d / %d", carspawnActiveLineIndex + 1, lineCount);
			draw_menu_line(caption, 350.0, 15.0, 18.0, 0.0, 5.0, false, true);
			for (int i = 0; i < itemCount; i++)
				if (strlen(vehicleModels[carspawnActiveLineIndex][i]))
					draw_menu_line(vehicleModels[carspawnActiveLineIndex][i], 100.0, 5.0, 200.0, 100.0 + i * 110.0, 5.0, i == carspawnActiveItemIndex, false, false);
			
			update_features();
			WAIT(0);
		} while (GetTickCount() < maxTickCount);
		waitTime = 0;

		bool bSelect, bBack, bUp, bDown, bLeft, bRight;
		get_button_state(&bSelect, &bBack, &bUp, &bDown, &bLeft, &bRight);
		
		if (bSelect)
		{
			menu_beep();
			LPCSTR modelName = vehicleModels[carspawnActiveLineIndex][carspawnActiveItemIndex];
			DWORD model = GAMEPLAY::GET_HASH_KEY((char *)modelName);
			if (STREAMING::IS_MODEL_IN_CDIMAGE(model) && STREAMING::IS_MODEL_A_VEHICLE(model))
			{
				STREAMING::REQUEST_MODEL(model);				
				while (!STREAMING::HAS_MODEL_LOADED(model)) WAIT(0);
				Vector3 coords = ENTITY::GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS(PLAYER::PLAYER_PED_ID(), 0.0, 5.0, 0.0);
				Vehicle veh = VEHICLE::CREATE_VEHICLE(model, coords.x, coords.y, coords.z, 0.0, 1, 1);
				VEHICLE::SET_VEHICLE_ON_GROUND_PROPERLY(veh);

				if (featureVehWrapInSpawned)
				{
					ENTITY::SET_ENTITY_HEADING(veh, ENTITY::GET_ENTITY_HEADING(PLAYER::PLAYER_PED_ID()));
					PED::SET_PED_INTO_VEHICLE(PLAYER::PLAYER_PED_ID(), veh, -1);
				}

				WAIT(0);
				STREAMING::SET_MODEL_AS_NO_LONGER_NEEDED(model);
				ENTITY::SET_VEHICLE_AS_NO_LONGER_NEEDED(&veh);

				char statusText[32];
				sprintf_s(statusText, "%s spawned", modelName);
				set_status_text(statusText);

				return true;
			}
		} else
		if (bBack)
		{
			menu_beep();
			break;
		} else
		if (bRight)
		{
			menu_beep();
			carspawnActiveItemIndex++;
			int itemsMax = (carspawnActiveLineIndex == (lineCount - 1)) ? itemCountLastLine : itemCount;
			if (carspawnActiveItemIndex == itemsMax) 
				carspawnActiveItemIndex = 0;			
			waitTime = 100;
		} else
		if (bLeft)
		{
			menu_beep();
			if (carspawnActiveItemIndex == 0) 
				carspawnActiveItemIndex = (carspawnActiveLineIndex == (lineCount - 1)) ? itemCountLastLine : itemCount;
			carspawnActiveItemIndex--;
			waitTime = 100;
		} else
		if (bUp)
		{
			menu_beep();
			if (carspawnActiveLineIndex == 0) 
				carspawnActiveLineIndex = lineCount;
			carspawnActiveLineIndex--;
			waitTime = 200;
		} else
		if (bDown)
		{
			menu_beep();
			carspawnActiveLineIndex++;
			if (carspawnActiveLineIndex == lineCount) 
				carspawnActiveLineIndex = 0;			
			waitTime = 200;
		}
		if (carspawnActiveLineIndex == (lineCount - 1))
			if (carspawnActiveItemIndex >= itemCountLastLine)
				carspawnActiveItemIndex = 0;
	}
	return false;
}



void rotate_camera(string msg)
{
	for(int i = 0; i < 360; i++)
	{
		Vector3 cam_rot = CAM::GET_CAM_ROT(lidarCam, 0);
		CAM::SET_CAM_ROT(lidarCam, cam_rot.x, cam_rot.y, cam_rot.z + 1.0, 0);
		CAM::RENDER_SCRIPT_CAMS(1, 3000, lidarCam, 0, 0);
		set_status_text("rotating!");
		WAIT(10);
	}
}


void process_veh_menu()
{
	const float lineWidth = 250.0;
	const int lineCount = 10;

	std::string caption = "VEHICLE OPTIONS";

	static struct {
		LPCSTR		text;
		bool		*pState;
		bool		*pUpdated;
	} lines[lineCount] = {
		{"CAR SPAWNER",		NULL, NULL},
		{"PAINT RANDOM",	NULL, NULL},
		{"FIX",				NULL, NULL},
		{"SEAT BELT",		&featureVehSeatbelt, &featureVehSeatbeltUpdated},
		{"WRAP IN SPAWNED",	&featureVehWrapInSpawned, NULL},
		{"INVINCIBLE",		&featureVehInvincible, &featureVehInvincibleUpdated},
		{"STRONG WHEELS",	&featureVehInvincibleWheels, &featureVehInvincibleWheelsUpdated},
		{"SPEED BOOST",		&featureVehSpeedBoost, NULL},	
		{"SHOW STATS",	    &featureVehShowStats, NULL},
		{"LET GAME DRIVE",	&featureVehShouldGameDrive, NULL}

	};

	DWORD waitTime = 150;
	while (true)
	{
		// timed menu draw, used for pause after active line switch
		DWORD maxTickCount = GetTickCount() + waitTime;
		do 
		{
			// draw menu
			draw_menu_line(caption, lineWidth, 15.0, 18.0, 0.0, 5.0, false, true);
			for (int i = 0; i < lineCount; i++)
				if (i != activeLineIndexVeh)
					draw_menu_line(line_as_str(lines[i].text, lines[i].pState), 
								   lineWidth, 9.0, 60.0 + i * 36.0, 0.0, 9.0, false, false);
			draw_menu_line(line_as_str(lines[activeLineIndexVeh].text, lines[activeLineIndexVeh].pState), 
						   lineWidth + 1.0, 11.0, 56.0 + activeLineIndexVeh * 36.0, 0.0, 7.0, true, false);

			update_features();
			WAIT(0);
		} while (GetTickCount() < maxTickCount);
		waitTime = 0;

		// process buttons
		bool bSelect, bBack, bUp, bDown;
		get_button_state(&bSelect, &bBack, &bUp, &bDown, NULL, NULL);
		if (bSelect)
		{
			menu_beep();

			// common variables
			BOOL bPlayerExists = ENTITY::DOES_ENTITY_EXIST(PLAYER::PLAYER_PED_ID());
			Player player = PLAYER::PLAYER_ID();
			playerPed = PLAYER::PLAYER_PED_ID();

			switch (activeLineIndexVeh)
			{
				case 0:
					if (process_carspawn_menu()) return;				
					break;
				case 1:
					if (bPlayerExists) 
					{
						if (PED::IS_PED_IN_ANY_VEHICLE(playerPed, 0))
						{
							Vehicle veh = PED::GET_VEHICLE_PED_IS_USING(playerPed);
							VEHICLE::SET_VEHICLE_CUSTOM_PRIMARY_COLOUR(veh, rand() % 255, rand() % 255, rand() % 255);
							if (VEHICLE::GET_IS_VEHICLE_PRIMARY_COLOUR_CUSTOM(veh))
								VEHICLE::SET_VEHICLE_CUSTOM_SECONDARY_COLOUR(veh, rand() % 255, rand() % 255, rand() % 255);
						} else
						{
							set_status_text("player isn't in a vehicle");
						}
					}
					break;
				case 2:
					if (bPlayerExists) 
						if (PED::IS_PED_IN_ANY_VEHICLE(playerPed, 0))
							VEHICLE::SET_VEHICLE_FIXED(PED::GET_VEHICLE_PED_IS_USING(playerPed));
						else
							set_status_text("player isn't in a vehicle");
					break;
//				case 8:
//					// self driving car "SELF DRIVING"
////					PLAYER::SET_ALL_RANDOM_PEDS_FLEE(player, true);
//					featureVehShowStats = ! featureVehShowStats;
//					showStatus = ! showStatus;
//					set_status_text("Toggling stats");

//					PLAYER::SET_ALL_RANDOM_PEDS_FLEE_THIS_FRAME(player);
//					if (bPlayerExists && PED::IS_PED_IN_ANY_VEHICLE(playerPed, 0))
//					{
//						
//						DWORD model = ENTITY::GET_ENTITY_MODEL(vehicle);
//
//
//
//						//set_status_text("SELF-DRIVING MODE    ...Is that you, Michael?");
//
//						bool useHoodCam = true;
//
//						if(useHoodCam)
//						{
////							CAM::;
//						}
//						else
//						{
//							//DWORD lidarCam = CAM::GET_RENDERING_CAM();s
//
//
//							lidarCam = CAM::CREATE_CAM("default_scripted_camera", 0);
//
////							lidarCam = CAM::CREATE_CAMERA_WITH_PARAMS("default_scripted_camera", 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0, 2);
//							//CAM::SET_CAM_NEAR_CLIP(a_0._f20, a_0._fC);
//							//CAM::POINT_CAM_AT_ENTITY(l_31D, PLAYER::PLAYER_PED_ID(), 0.0606, 0.04, 0.0745, 0);
//							//                                      x      y     z
////							CAM::ATTACH_CAM_TO_ENTITY(lidarCam, vehicle, -2.4646, 2.427, 1.3, 1);
////							CAM::ATTACH_CAM_TO_ENTITY(lidarCam, playerPed,     0.0,   0.0, 1.0, 1);
////							CAM::ATTACH_CAM_TO_PED_BONE(lidarCam, playerPed, 0, 0.0, 0.0, 1.0, 1);
////							CAM::SET_CAM_INHERIT_ROLL_VEHICLE(lidarCam, vehicle);
////							Vector3 coords = CAM::GET_CAM_COORD(lidarCam);
////							set_status_text(
////								"lidarCam coords:" + std::to_string(coords.x) 
////											  + std::to_string(coords.y)
////											  + std::to_string(coords.z));
//							// /Users/cq/Downloads/gtav.ysc.decompiled/cam_control.c:
//							// if (!CAM::DOES_CAM_EXIST(l_31D)) {
//							//    12          l_31D = CAM::CREATE_CAMERA_WITH_PARAMS(0x19286a9, v_2, v_5, 50.0, 0, 2);
//							//    13:         CAM::ATTACH_CAM_TO_ENTITY(l_31D, PLAYER::PLAYER_PED_ID(), -2.4646, 2.427, 1.3, 1);
//							//    14          CAM::POINT_CAM_AT_ENTITY(l_31D, PLAYER::PLAYER_PED_ID(), 0.0606, 0.04, 0.0745, 0);
//							//    15          CAM::SET_CAM_FOV(l_31D, 50.0);
//							// /Users/cq/Downloads/gtav.ysc.decompiled/b372d/agency_heist3a.c4:
//							//  63793          }
//							//  63794          v_1A._f2 = a_0._f24;
//							//  63795:         CAM::ATTACH_CAM_TO_ENTITY(a_0._f4, a_0._f3, v_1A, 1);
//							//  63796          CAM::POINT_CAM_AT_ENTITY(a_0._f4, a_0._f3, ({0.0, v_11._f1, 0.6 }) + a_0._f34, 1);
//							//  63797          CAM::SET_CAM_PARAMS(a_0._f4, v_17, 0.0, 0.0, 0.0, a_0._f33, 0, 1, 1, 2);
//							//  .....
//							//  82011              a_0._f1 = CAM::CREATE_CAMERA(0x19286a9, 1);
//							//  82012              if (a_1) {
//							//  82013:                 CAM::ATTACH_CAM_TO_ENTITY(a_0._f1, a_0._f5, v_C, 1);
//							//  82014              } else { 
//							//  82015                  CAM::SET_CAM_COORD(a_0._f1, v_9);
//							//  .....
//							//  82019              a_0._f2 = CAM::CREATE_CAMERA(0x19286a9, 1);
//							//  82020              if (a_1 && (!a_4)) {
//							//  82021:                 CAM::ATTACH_CAM_TO_ENTITY(a_0._f2, a_0._f5, v_C + (v_1B * ((vector)a_0._fC)), 1);
//							//  82022              } else { 
//							//  82023                  CAM::SET_CAM_COORD(a_0._f2, v_9 + (v_18 * ((vector)a_0._fC)));
//							//  .....
//							//  82028                  a_0._f4 = CAM::CREATE_CAMERA(0x19286a9, 1);
//							//  82029                  if (a_1 && (!a_4)) {
//							//  82030:                     CAM::ATTACH_CAM_TO_ENTITY(a_0._f4, a_0._f5, v_C + ((v_1B * ((vector)a_0._fC)) * ((vector)a_0._fD)), 1);
//							//  82031                  } else { 
//							//  82032                      CAM::SET_CAM_COORD(a_0._f4, v_9 + ((v_18 * ((vector)a_0._fC)) * ((vector)a_0._fD)));
//							//CAM::SET_CAM_COORD(lidarCam, coords.x, coords.y * 2.0, coords.z);
//	//						set_status_text("changed lidarCam coords y:" + std::to_string(coords.y));
//
//							if (CAM::DOES_CAM_EXIST(lidarCam)) {
//								if (!CAM::IS_CAM_ACTIVE(lidarCam)) {
//									CAM::SET_CAM_ACTIVE(lidarCam, 1);
//									CAM::RENDER_SCRIPT_CAMS(1, 3000, lidarCam, 1, 0);
//
//									//(BOOL render, BOOL ease, Any camera, BOOL p3, BOOL p4) { invoke<Void>(0x07E5B515DB0636FC, render, ease, camera, p3, p4);
//
//									// SET_CAM_POINT_DAMPING_PARAMS - Can we attach to vehichle
//
//									// IS_CAM_COLLIDING - give it physics ?
//
//									// SET_CAM_INHERIT_ROLL_PED
//
//									// SET_CAM_INHERIT_ROLL_VEHICLE
//
//									// SET_CAM_PROPAGATE
//
//									// SET_CAM_INHERIT_ROLL_OBJECT
//
//									// GAMEPLAY::SET_TIME_SCALE(1.0)
//
//									// CAMERA INTERPOLATION AS WELL to reduce shock absorbing
//
//									// CAM_IS_SPHERE_VISIBLE
//
//								}
//							}
//						}
//					} 
//					else 
//					{
//						set_status_text("get a ride!");
//						CAM::RENDER_SCRIPT_CAMS(0, 3000, 0, 0, 0);
//					}
//					break;
				// switchable features
//				case 9:
//					shouldWander = !shouldWander;
//					*lines[activeLineIndexVeh].pState = !(*lines[activeLineIndexVeh].pState);
//					break;
				default:
					if (lines[activeLineIndexVeh].pState)
						*lines[activeLineIndexVeh].pState = !(*lines[activeLineIndexVeh].pState);
					if (lines[activeLineIndexVeh].pUpdated)
						*lines[activeLineIndexVeh].pUpdated = true;					
			}
			waitTime = 200;
		} else
		if (bBack || trainer_switch_pressed())
		{
			menu_beep();
			break;
		} else
		if (bUp)
		{
			menu_beep();
			if (activeLineIndexVeh == 0) 
				activeLineIndexVeh = lineCount;
			activeLineIndexVeh--;
			waitTime = 150;
		} else
		if (bDown)
		{
			menu_beep();
			activeLineIndexVeh++;
			if (activeLineIndexVeh == lineCount) 
				activeLineIndexVeh = 0;			
			waitTime = 150;
		}
	}
}

int activeLineIndexWorld = 0;

void process_world_menu()
{
	const float lineWidth = 250.0;
	const int lineCount = 5;

	std::string caption = "WORLD  OPTIONS";

	static struct {
		LPCSTR		text;
		bool		*pState;
		bool		*pUpdated;
	} lines[lineCount] = {
		{"MOON GRAVITY",	&featureWorldMoonGravity,	NULL},
		{"RANDOM COPS",		&featureWorldRandomCops,	NULL},
		{"RANDOM TRAINS",	&featureWorldRandomTrains,	NULL},
		{"RANDOM BOATS",	&featureWorldRandomBoats,	NULL},
		{"GARBAGE TRUCKS",	&featureWorldGarbageTrucks,	NULL}
	};

	DWORD waitTime = 150;
	while (true)
	{
		// timed menu draw, used for pause after active line switch
		DWORD maxTickCount = GetTickCount() + waitTime;
		do 
		{
			// draw menu
			draw_menu_line(caption, lineWidth, 15.0, 18.0, 0.0, 5.0, false, true);
			for (int i = 0; i < lineCount; i++)
				if (i != activeLineIndexWorld)
					draw_menu_line(line_as_str(lines[i].text, lines[i].pState), 
					lineWidth, 9.0, 60.0 + i * 36.0, 0.0, 9.0, false, false);
			draw_menu_line(line_as_str(lines[activeLineIndexWorld].text, lines[activeLineIndexWorld].pState), 
				lineWidth + 1.0, 11.0, 56.0 + activeLineIndexWorld * 36.0, 0.0, 7.0, true, false);

			update_features();
			WAIT(0);
		} while (GetTickCount() < maxTickCount);
		waitTime = 0;

		// process buttons
		bool bSelect, bBack, bUp, bDown;
		get_button_state(&bSelect, &bBack, &bUp, &bDown, NULL, NULL);
		if (bSelect)
		{
			menu_beep();
			switch (activeLineIndexWorld)
			{
				case 0: 
					featureWorldMoonGravity = !featureWorldMoonGravity;
					GAMEPLAY::SET_GRAVITY_LEVEL(featureWorldMoonGravity ? 2 : 0);
					break;
				case 1: 
					// featureWorldRandomCops being set in update_features
					PED::SET_CREATE_RANDOM_COPS(!featureWorldRandomCops);
					break;
				case 2:
					featureWorldRandomTrains = !featureWorldRandomTrains;
					VEHICLE::SET_RANDOM_TRAINS(featureWorldRandomTrains);
					break;
				case 3:
					featureWorldRandomBoats = !featureWorldRandomBoats;
					VEHICLE::SET_RANDOM_BOATS(featureWorldRandomBoats);
					break;
				case 4:
					featureWorldGarbageTrucks = !featureWorldGarbageTrucks;
					VEHICLE::SET_GARBAGE_TRUCKS(featureWorldGarbageTrucks);
					break;
			}
			waitTime = 200;
		} else
		if (bBack || trainer_switch_pressed())
		{
			menu_beep();
			break;
		} else
		if (bUp)
		{
			menu_beep();
			if (activeLineIndexWorld == 0) 
				activeLineIndexWorld = lineCount;
			activeLineIndexWorld--;
			waitTime = 150;
		} else
		if (bDown)
		{
			menu_beep();
			activeLineIndexWorld++;
			if (activeLineIndexWorld == lineCount) 
				activeLineIndexWorld = 0;			
			waitTime = 150;
		}
	}
}

int activeLineIndexTime = 0;

void process_time_menu()
{
	const float lineWidth = 250.0;
	const int lineCount = 4;

	std::string caption = "TIME  OPTIONS";

	static struct {
		LPCSTR		text;
		bool		*pState;
		bool		*pUpdated;
	} lines[lineCount] = {
		{"HOUR FORWARD",	 NULL,				 NULL},
		{"HOUR BACKWARD",	 NULL,				 NULL},
		{"CLOCK PAUSED",	 &featureTimePaused, &featureTimePausedUpdated},
		{"SYNC WITH SYSTEM", &featureTimeSynced, NULL}
	};

	DWORD waitTime = 150;
	while (true)
	{
		// timed menu draw, used for pause after active line switch
		DWORD maxTickCount = GetTickCount() + waitTime;
		do 
		{
			// draw menu
			draw_menu_line(caption, lineWidth, 15.0, 18.0, 0.0, 5.0, false, true);
			for (int i = 0; i < lineCount; i++)
				if (i != activeLineIndexTime)
					draw_menu_line(line_as_str(lines[i].text, lines[i].pState), 
					lineWidth, 9.0, 60.0 + i * 36.0, 0.0, 9.0, false, false);
			draw_menu_line(line_as_str(lines[activeLineIndexTime].text, lines[activeLineIndexTime].pState), 
				lineWidth + 1.0, 11.0, 56.0 + activeLineIndexTime * 36.0, 0.0, 7.0, true, false);

			update_features();
			WAIT(0);
		} while (GetTickCount() < maxTickCount);
		waitTime = 0;

		// process buttons
		bool bSelect, bBack, bUp, bDown;
		get_button_state(&bSelect, &bBack, &bUp, &bDown, NULL, NULL);
		if (bSelect)
		{
			menu_beep();
			switch (activeLineIndexTime)
			{
			// hour forward/backward
			case 0: 
			case 1:
				{
					int h = TIME::GET_CLOCK_HOURS();
					if (activeLineIndexTime == 0) h = (h == 23) ? 0 : h + 1; else h = (h == 0) ? 23 : h - 1;
					int m = TIME::GET_CLOCK_MINUTES();
					TIME::SET_CLOCK_TIME(h, m, 0);
					char text[32];
					sprintf_s(text, "time %d:%d", h, m);
					set_status_text(text);
				}
				break;
			// switchable features
			default:
				if (lines[activeLineIndexTime].pState)
					*lines[activeLineIndexTime].pState = !(*lines[activeLineIndexTime].pState);
				if (lines[activeLineIndexTime].pUpdated)
					*lines[activeLineIndexTime].pUpdated = true;	
			}
			waitTime = 200;
		} else
		if (bBack || trainer_switch_pressed())
		{
			menu_beep();
			break;
		} else
		if (bUp)
		{
			menu_beep();
			if (activeLineIndexTime == 0) 
				activeLineIndexTime = lineCount;
			activeLineIndexTime--;
			waitTime = 150;
		} else
		if (bDown)
		{
			menu_beep();
			activeLineIndexTime++;
			if (activeLineIndexTime == lineCount) 
				activeLineIndexTime = 0;			
			waitTime = 150;
		}
	}
}

int activeLineIndexWeather = 0;

void process_weather_menu()
{
	const float lineWidth = 250.0;
	const int lineCount = 16;

	std::string caption = "WEATHER  OPTIONS";

	static struct {
		LPCSTR		text;
		bool		*pState;
		bool		*pUpdated;
	} lines[lineCount] = {
		{"WIND",	 	 &featureWeatherWind,	NULL},
		{"SET PERSIST",  &featureWeatherPers,	NULL},
		{"EXTRASUNNY",	 NULL,					NULL},
		{"CLEAR",		 NULL,					NULL},
		{"CLOUDS",		 NULL,					NULL},
		{"SMOG",		 NULL,					NULL},
		{"FOGGY",	 	 NULL,					NULL},
		{"OVERCAST",	 NULL,					NULL},
		{"RAIN",		 NULL,					NULL},
		{"THUNDER",		 NULL,					NULL},
		{"CLEARING",	 NULL,					NULL},
		{"NEUTRAL",		 NULL,					NULL},
		{"SNOW",		 NULL,					NULL},
		{"BLIZZARD",	 NULL,					NULL},
		{"SNOWLIGHT",	 NULL,					NULL},
		{"XMAS",		 NULL,					NULL}
	};


	DWORD waitTime = 150;
	while (true)
	{
		// timed menu draw, used for pause after active line switch
		DWORD maxTickCount = GetTickCount() + waitTime;
		do 
		{
			// draw menu
			draw_menu_line(caption, lineWidth, 15.0, 18.0, 0.0, 5.0, false, true);
			for (int i = 0; i < lineCount; i++)
				if (i != activeLineIndexWeather)
					draw_menu_line(line_as_str(lines[i].text, lines[i].pState), 
					lineWidth, 9.0, 60.0 + i * 36.0, 0.0, 9.0, false, false);
			draw_menu_line(line_as_str(lines[activeLineIndexWeather].text, lines[activeLineIndexWeather].pState), 
				lineWidth + 1.0, 11.0, 56.0 + activeLineIndexWeather * 36.0, 0.0, 7.0, true, false);

			update_features();
			WAIT(0);
		} while (GetTickCount() < maxTickCount);
		waitTime = 0;

		// process buttons
		bool bSelect, bBack, bUp, bDown;
		get_button_state(&bSelect, &bBack, &bUp, &bDown, NULL, NULL);
		if (bSelect)
		{
			menu_beep();
			switch (activeLineIndexWeather)
			{
			// wind
			case 0: 
				featureWeatherWind = !featureWeatherWind;
				if (featureWeatherWind)
				{
					GAMEPLAY::SET_WIND(1.0);
					GAMEPLAY::SET_WIND_SPEED(11.99);
					GAMEPLAY::SET_WIND_DIRECTION(ENTITY::GET_ENTITY_HEADING(PLAYER::PLAYER_PED_ID()));
				} else
				{
					GAMEPLAY::SET_WIND(0.0);
					GAMEPLAY::SET_WIND_SPEED(0.0);
				}
				break;
			// set persist
			case 1:
				featureWeatherPers = !featureWeatherPers;
				break;
			// set weather
			default:
				GAMEPLAY::CLEAR_OVERRIDE_WEATHER();
				if (featureWeatherPers)
				{
					GAMEPLAY::SET_OVERRIDE_WEATHER((char *)lines[activeLineIndexWeather].text);
				}
				else
				{
					GAMEPLAY::SET_WEATHER_TYPE_NOW_PERSIST((char *)lines[activeLineIndexWeather].text);
					GAMEPLAY::CLEAR_WEATHER_TYPE_PERSIST();
				}				
				set_status_text(lines[activeLineIndexWeather].text);
			}
			waitTime = 200;
		} else
		if (bBack || trainer_switch_pressed())
		{
			menu_beep();
			break;
		} else
		if (bUp)
		{
			menu_beep();
			if (activeLineIndexWeather == 0) 
				activeLineIndexWeather = lineCount;
			activeLineIndexWeather--;
			waitTime = 150;
		} else
		if (bDown)
		{
			menu_beep();
			activeLineIndexWeather++;
			if (activeLineIndexWeather == lineCount) 
				activeLineIndexWeather = 0;			
			waitTime = 150;
		}
	}
}

int activeLineIndexMisc = 0;

void process_misc_menu()
{
	const float lineWidth = 250.0;
	const int lineCount = 2;

	std::string caption = "MISC  OPTIONS";

	static struct {
		LPCSTR		text;
		bool		*pState;
		bool		*pUpdated;
	} lines[lineCount] = {
		{"NEXT RADIO TRACK",	NULL,					NULL},
		{"HIDE HUD",			&featureMiscHideHud,	NULL}		
	};


	DWORD waitTime = 150;
	while (true)
	{
		// timed menu draw, used for pause after active line switch
		DWORD maxTickCount = GetTickCount() + waitTime;
		do 
		{
			// draw menu
			draw_menu_line(caption, lineWidth, 15.0, 18.0, 0.0, 5.0, false, true);
			for (int i = 0; i < lineCount; i++)
				if (i != activeLineIndexMisc)
					draw_menu_line(line_as_str(lines[i].text, lines[i].pState), 
					lineWidth, 9.0, 60.0 + i * 36.0, 0.0, 9.0, false, false);
			draw_menu_line(line_as_str(lines[activeLineIndexMisc].text, lines[activeLineIndexMisc].pState), 
				lineWidth + 1.0, 11.0, 56.0 + activeLineIndexMisc * 36.0, 0.0, 7.0, true, false);

			update_features();
			WAIT(0);
		} while (GetTickCount() < maxTickCount);
		waitTime = 0;

		// process buttons
		bool bSelect, bBack, bUp, bDown;
		get_button_state(&bSelect, &bBack, &bUp, &bDown, NULL, NULL);
		if (bSelect)
		{
			menu_beep();
			switch (activeLineIndexMisc)
			{
			// next radio track
			case 0: 
				if (ENTITY::DOES_ENTITY_EXIST(PLAYER::PLAYER_PED_ID()) && 
					PED::IS_PED_IN_ANY_VEHICLE(PLAYER::PLAYER_PED_ID(), 0))
						AUDIO::SKIP_RADIO_FORWARD();
				break;
			// switchable features
			default:
				if (lines[activeLineIndexMisc].pState)
					*lines[activeLineIndexMisc].pState = !(*lines[activeLineIndexMisc].pState);
				if (lines[activeLineIndexMisc].pUpdated)
					*lines[activeLineIndexMisc].pUpdated = true;
			}
			waitTime = 200;
		} else
		if (bBack || trainer_switch_pressed())
		{
			menu_beep();
			break;
		} else
		if (bUp)
		{
			menu_beep();
			if (activeLineIndexMisc == 0) 
				activeLineIndexMisc = lineCount;
			activeLineIndexMisc--;
			waitTime = 150;
		} else
		if (bDown)
		{
			menu_beep();
			activeLineIndexMisc++;
			if (activeLineIndexMisc == lineCount) 
				activeLineIndexMisc = 0;			
			waitTime = 150;
		}
	}
}

int activeLineIndexMain = 0;

void process_main_menu()
{
	const float lineWidth = 250.0;
	const int lineCount = 7;	

	std::string caption = "NATIVE  TRAINER  (AB)";

	static LPCSTR lineCaption[lineCount] = {
		"PLAYER",
		"WEAPON",
		"VEHICLE",
		"WORLD",
		"TIME",
		"WEATHER",
		"MISC"
	};

	DWORD waitTime = 150;
	while (true)
	{
		// timed menu draw, used for pause after active line switch
		DWORD maxTickCount = GetTickCount() + waitTime;
		do 
		{
			// draw menu
			draw_menu_line(caption, lineWidth, 15.0, 18.0, 0.0, 5.0, false, true);
			for (int i = 0; i < lineCount; i++)
				if (i != activeLineIndexMain)
					draw_menu_line(lineCaption[i], lineWidth, 9.0, 60.0 + i * 36.0, 0.0, 9.0, false, false);
			draw_menu_line(lineCaption[activeLineIndexMain], lineWidth + 1.0, 11.0, 56.0 + activeLineIndexMain * 36.0, 0.0, 7.0, true, false);
			
			update_features();
			WAIT(0);
		} while (GetTickCount() < maxTickCount);
		waitTime = 0;

		// process buttons
		bool bSelect, bBack, bUp, bDown;
		get_button_state(&bSelect, &bBack, &bUp, &bDown, NULL, NULL);
		if (bSelect)
		{
			menu_beep();
			switch (activeLineIndexMain)
			{
				case 0:
					process_player_menu();					
					break;
				case 1:
					process_weapon_menu();
					break;
				case 2:
					process_veh_menu();
					break;
				case 3:
					process_world_menu();
					break;
				case 4:
					process_time_menu();
					break;
				case 5:
					process_weather_menu();
					break;
				case 6:
					process_misc_menu();
					break;
			}
			waitTime = 200;
		} else
		if (bBack || trainer_switch_pressed())
		{
			menu_beep();
			break;
		} else
		if (bUp)
		{
			menu_beep();
			if (activeLineIndexMain == 0) 
				activeLineIndexMain = lineCount;
			activeLineIndexMain--;
			waitTime = 150;
		} else
		if (bDown)
		{
			menu_beep();
			activeLineIndexMain++;
			if (activeLineIndexMain == lineCount) 
				activeLineIndexMain = 0;			
			waitTime = 150;
		}
	}
}

void reset_globals()
{
	activeLineIndexMain			=
	activeLineIndexPlayer		=
	skinchangerActiveLineIndex	=
	skinchangerActiveItemIndex	=
	teleportActiveLineIndex		=
	activeLineIndexWeapon		=
	activeLineIndexVeh			=
	carspawnActiveLineIndex		=
	carspawnActiveItemIndex		=
	activeLineIndexWorld		=
	activeLineIndexWeather		=	0;

	featurePlayerInvincible				=
	featurePlayerInvincibleUpdated		=
	featurePlayerNeverWanted			=
	featurePlayerIgnored				=
	featurePlayerIgnoredUpdated			=
	featurePlayerUnlimitedAbility		=
	featurePlayerNoNoise				=
	featurePlayerNoNoiseUpdated			=
	featurePlayerFastSwim				=
	featurePlayerFastSwimUpdated		=
	featurePlayerFastRun				=
	featurePlayerFastRunUpdated			=
	featurePlayerSuperJump				=
	featureWeaponNoReload				=
	featureWeaponFireAmmo				=
	featureWeaponExplosiveAmmo			=
	featureWeaponExplosiveMelee			=
	featureWeaponVehRockets				=
	featureVehInvincible				=
	featureVehInvincibleUpdated			=
	featureVehInvincibleWheels			=
	featureVehInvincibleWheelsUpdated	=
	featureVehSeatbelt					=
	featureVehSeatbeltUpdated			=
	featureVehSpeedBoost				=
	featureVehShowStats				    =
	featureVehShouldGameDrive      	    =
	featureVehWrapInSpawned				=
	featureWorldMoonGravity				=
	featureTimePaused					=
	featureTimePausedUpdated			=
	featureTimeSynced					=
	featureWeatherWind					=
	featureWeatherPers					=
	featureMiscLockRadio				=
	featureMiscHideHud					=	false;

	featureWorldRandomCops		=
	featureWorldRandomTrains	=
	featureWorldRandomBoats		=
	featureWorldGarbageTrucks	=	true;

	skinchanger_used			=	false;

	shouldRunAlignCam = true;

	shouldSelfDrive = true;
}

void main()
{	
	reset_globals();

//	alignCameraThread = &(std::thread(alignCamera));

	while (true)
	{
		if (trainer_switch_pressed())
		{
			menu_beep();
			process_main_menu();
		}

		update_features();
		WAIT(0);
	}
}

void Cleanup()
{
	shouldRunAlignCam = false;
	shouldSelfDrive = false;
	logFile.close();
//	(*alignCameraThread).join();
//	delete alignCameraThread;
}

UINT depthViewLimitLow = 3; // TODO: pretty sure we just need 4 here.
UINT depthViewLimitHigh = 8;

int createDepthViewCount = 0;
void readDepthViewLimits()
{
	createDepthViewCount = 0;
	std::ifstream infile("depthViewLimits.txt");
	
	int a, b;
	infile >> a >> b;
	depthViewLimitLow = a;
	depthViewLimitHigh = b;
}

bool cqOBSCreateDepthhooked = false;
typedef void (*DOCREATEDEPTHPROC)(ID3D11Device*);
HookData devCreateDepthViewHook;

typedef HRESULT(STDMETHODCALLTYPE *D3D11CreateDepthHookPROC)(ID3D11Device *device, ID3D11Resource *pResource,
	const D3D11_DEPTH_STENCIL_VIEW_DESC *pDesc, ID3D11DepthStencilView **ppDepthStencilView);
HRESULT STDMETHODCALLTYPE D3D11CreateDepthHook(ID3D11Device *device, ID3D11Resource *pResource,
		const D3D11_DEPTH_STENCIL_VIEW_DESC *pDesc, ID3D11DepthStencilView **ppDepthStencilView)
{
	logFile << "depth hook #" << createDepthViewCount << endl;
	createDepthViewCount += 1;
	if( createDepthViewCount >= depthViewLimitLow && createDepthViewCount < depthViewLimitHigh )
	{
		logFile << "saving depth hook #" << createDepthViewCount << endl;
		saveDepthResource(pResource);
		if(createDepthViewCount == (depthViewLimitHigh - 1))
		{
			cqOBSCreateDepthhooked = true;
		}
	}

	devCreateDepthViewHook.Unhook();
	HRESULT hRes = device->CreateDepthStencilView(pResource, pDesc, ppDepthStencilView);
	devCreateDepthViewHook.Rehook();
	return hRes;
}

bool first_present = true;

bool isValidDegree(float val)
{
	return (val >= -360.0 && val <= 360.0);
}

int frame_i = 0;

void alignCameraOnce()
{
//		frame_i += 1;
//		set_status_text("aligning " + std::to_string(frame_i));
		try
		{
			if(vehicle != -1 && lidarCam != NULL)
			{
	//			frame_i = 0;
				float x = 0, y, z, w;
//				ENTITY::GET_ENTITY_QUATERNION(vehicle, &x, &y, &z, &w);
				Vector3 vehCoords = ENTITY::GET_ENTITY_COORDS(vehicle, true);
	
				// Roll, pitch, heading cause imperfect rotation of camera at vehicle coords.
//				float vehRoll    = ENTITY::GET_ENTITY_ROLL(vehicle);
//				float vehPitch   = ENTITY::GET_ENTITY_PITCH(vehicle);
//				float vehHeading = ENTITY::GET_ENTITY_HEADING(vehicle);

				Vector3 vehRotation = ENTITY::GET_ENTITY_ROTATION(vehicle, 0);  // Causes perfect rotation of camera at vehicle coords 
//				
				float zDelta = 1.0;

				// TODO: Add zDelta
				DirectX::XMVECTORF32 start = { 0, 0, zDelta };
				DirectX::XMVECTOR _end;
				DirectX::XMFLOAT4A end;

				DirectX::XMVECTOR rotationQuaternion = DirectX::XMQuaternionRotationRollPitchYaw(
					DirectX::XMConvertToRadians(vehRotation.x),              
					DirectX::XMConvertToRadians(vehRotation.y),              
					DirectX::XMConvertToRadians(vehRotation.z)); 

//				int shellIndex = ENTITY::_GET_ENTITY_BONE_INDEX(vehicle, "bodyshell");
//				int chassisIndex = ENTITY::_GET_ENTITY_BONE_INDEX(vehicle, "bodyshell");
//				float uprightValue = ENTITY::GET_ENTITY_UPRIGHT_VALUE(vehicle);

				DirectX::XMMATRIX rotationMatrix = DirectX::XMMatrixRotationQuaternion(rotationQuaternion);
				_end = DirectX::XMVector3TransformCoord(start, rotationMatrix);
				DirectX::XMStoreFloat4A( &end, _end );
//
//				DirectX::XMVECTOR rollPitchQuaternion = DirectX::XMQuaternionRotationRollPitchYaw(
//					DirectX::XMConvertToRadians(vehRoll),       
//					DirectX::XMConvertToRadians(vehPitch),      
//					DirectX::XMConvertToRadians(0));            
//				DirectX::XMMATRIX headingRotationMatrix = DirectX::XMMatrixRotationQuaternion(headingQuaternion);
//				DirectX::XMMATRIX rollPitchRotationMatrix = DirectX::XMMatrixRotationQuaternion(rollPitchQuaternion);
////				_end = DirectX::XMVector3TransformCoord(start, headingRotationMatrix);
//				_end = DirectX::XMVector3TransformCoord(_end, rollPitchRotationMatrix);
//				DirectX::XMStoreFloat4A( &end, _end );
//
//				// TODO: Go from car coordinates to world coordinates with translation matrix
//
//				unsigned long a, b, c, d;
//				Vector3 forwardVector = ENTITY::GET_ENTITY_FORWARD_VECTOR(vehicle);
//
//				DirectX::XMVECTORF32 startForward = {forwardVector.x, forwardVector.y, forwardVector.z };
//				// Rotate forward vector 90 degrees yaw, and rotate by roll to get up vector
//				DirectX::XMMATRIX upVectorRotationMatrix = DirectX::XMMatrixRotationQuaternion(
//					DirectX::XMQuaternionRotationRollPitchYaw(0, M_PI_2, 0));
//				_end = DirectX::XMVector3TransformCoord(startForward, upVectorRotationMatrix);
//
//				upVectorRotationMatrix = DirectX::XMMatrixRotationQuaternion(
//					DirectX::XMQuaternionRotationRollPitchYaw(0, 0, M_PI_2));
//				_end = DirectX::XMVector3TransformCoord(_end, upVectorRotationMatrix);
//				DirectX::XMStoreFloat4A( &end, _end );

				CAM::SET_CAM_COORD(lidarCam,
					vehCoords.x + end.x,  // + - - + 
					vehCoords.y + end.y,  // + + - -
					vehCoords.z + end.z);
//
//				if (frame_i == 0)
//				{
//					CAM::SET_CAM_COORD(lidarCam,
//						vehCoords.x,
//						vehCoords.y,
//						vehCoords.z);				
//				}
//				ENTITY::ATTACH_ENTITY_TO_ENTITY_PHYSICALLY(lidarCam, vehicle, 0 /*int boneIndex */, 
//					0 /*int boneIndex2*/, 0 /*float x1*/, 0 /*float y1*/, 0 /*float z1*/, 0 /*float x2*/, 0 /*float y2*/,
//					0 /* float z2 */, 0 /*float xRot*/, 0 /*float yRot */, 0 /*float zRot*/, 10 /*float forceToBreak*/, 1, 1, 0, 1, 2);

				CAM::SET_CAM_ROT(lidarCam, vehRotation.x, vehRotation.y, vehRotation.z, 2);

				set_status_text(
//				      " r: "  + std::to_string(vehRoll) 
//					+ " p: "  + std::to_string(vehPitch)
//					+ " h: "  + std::to_string(vehHeading)
					+ "\nrx: " + std::to_string(vehRotation.x)
					+ " ry: "  + std::to_string(vehRotation.y)
					+ " rz: "  + std::to_string(vehRotation.z)
//					+ "\nsx: " + std::to_string(vehCoords.x)
//					+ " sy: "  + std::to_string(vehCoords.y)
//					+ " sz: "  + std::to_string(vehCoords.z)
					+ "\nex: " + std::to_string(end.x)
					+ " ey: "  + std::to_string(end.y)
					+ " ez: "  + std::to_string(end.z)
//					+ "\na: " + std::to_string(forwardVector.x)
//					+ "\nb: "  + std::to_string(forwardVector.y)
//					+ "\nc: "  + std::to_string(forwardVector.z)
				);

//				ENTITY::SET_ENTITY_HEADING(lidarCam, 90.0);
				frame_i += 1;
				return;

//				CAM::SET_CAM_ROT(lidarCam, vehRoll, vehPitch, vehHeading, 0);
	//
	//			set_status_text(
	//			   "vehicle quaternion  x: " + std::to_string(x) 
	//							    + " y: " + std::to_string(y)
	//							    + " z: " + std::to_string(z)
	//							    + " w: " + std::to_string(w));
	//
	//			ENTITY::SET_ENTITY_QUATERNION(lidarCam, x, y, z, w);
	//
//				CAM::SET_CAM_ANIM_CURRENT_PHASE(lidarCam, 0);
				//ENTITY::SET_ENTITY_ROTATION(lidarCam, vehPitch, vehRoll, vehHeading, 0, false) // Rotational velocity
				
//				float camX, camY, camZ;
//				float rotDeltaX, rotDeltaY, rotDeltaZ;
				
				Vector3 camRotation = CAM::GET_CAM_ROT(lidarCam, 0);
//				camX = vehCoords.x + SYSTEM::COS(vehRotation.y) * zDelta;
//				camZ = vehCoords.z + (zDelta - SYSTEM::SIN(vehRotation.y)) * zDelta;
//
//				camY  = vehCoords.y + SYSTEM::COS(vehRotation.x) * zDelta;
//				camZ += vehCoords.z + (zDelta - SYSTEM::SIN(vehRotation.x)) * zDelta;				
				// Get directx rotation matrix
				// Multiply camera point (0, 0, 1) by the rotation matrix
				// Add the vehicle coords to the rotated point

//				CAM::SET_CAM_COORD(lidarCam,
//					vehCoords.x + end.x,
//					vehCoords.y + end.y,
//					vehCoords.z + end.z);

//				CAM::SET_CAM_ROT(lidarCam, 0.0, 0.0, 0.0, 0);
//				CAM::SET_CAM_ROT(lidarCam, camRotation.x, camRotation.y, vehHeading, 0);
//				CAM::SET_CAM_ROT(lidarCam, vehRoll, vehPitch, vehHeading, 1);
//				if(isValidDegree(vehRoll) && isValidDegree(vehPitch) && isValidDegree(vehHeading))
//				{
//
//					
//				}

			}
		} catch (...) {}
}

void PresentHooky(void *swapAny){
	
//	alignCameraOnce();

	if(first_present)
	{
		HRESULT hRes;
		IDXGISwapChain * swap = static_cast<IDXGISwapChain *>(swapAny);
		logFile.open("native-trainver-log.txt");
//		logFile << "first present" << endl;
		readDepthViewLimits();
	
		ID3D11Device* dev = NULL;
		hRes = swap->GetDevice(__uuidof(ID3D11Device), (void**)&dev);
		UPARAM *vtableDev = *(UPARAM**)dev;
		devCreateDepthViewHook.Hook((FARPROC)*(vtableDev+(10)), (FARPROC)D3D11CreateDepthHook);	// CreateDepthStencilView
        SafeRelease(swap);
        SafeRelease(dev);
		devCreateDepthViewHook.Rehook();

		// TODO: ResizeBuffers hook
//
////	//	setupPresentHook(swap);
//		setupCreateDepthStencilViewHook(dev);
//
////		ID3D11DeviceContext *devcon;
////		dev->GetImmediateContext(&devcon);
//
//		
		first_present = false;
		presentCallbackUnregister(PresentHooky);
	}
//	else if(cqOBSCreateDepthhooked) 
//	{
//		copyDepthResources(swap);
//	}
//	SafeRelease(swap);
}

float* getDepthsAsFloats(unsigned height, unsigned width, BYTE* depths)
{
	bool print = false;
	static float* fDepths;
	int fDepthsLength = height * width;
	fDepths = new float[fDepthsLength];
	for (int j = 0; j < fDepthsLength; j++)
	{
		// initialize to infinite depth
		fDepths[j] = 0.0;
	}

	for (int y = 0; y < height; y++) 
	{
		for (int x = 0; x < width; x++) 
		{
			//double n = fDepths[y * width + x];
			//double top = n - *minDepth;

			float f;
			int fDepthsIndex = y * width + x;
			int i = fDepthsIndex * 4; // 4 bytes per float
			unsigned char b[] = { depths[i], depths[i + 1], depths[i + 2], depths[i + 3] };
			memcpy(&f, &b, sizeof(f));
			fDepths[fDepthsIndex] = f;

		}
	}
	return fDepths;
}

void ScriptMain()
{
	srand(GetTickCount());
	main();
}

void AlignCameraLoop()
{
	int i = 1;
//	frame_i += 1;
	while(shouldRunAlignCam)
	{
		WAIT(10);
		alignCameraOnce();
	}

}

float CHILIAD_X = 1722.844727;
float CHILIAD_Y = 6407.962891;
float CHILIAD_Z = 33.628269;

double TREVOR_ROAD_END_X = 2054.696533;
double TREVOR_ROAD_END_Y = 3729.036133;
double TREVOR_ROAD_END_Z = 32.656715;

double WINDY_END_X = -2090.888184;
double WINDY_END_Y = 2298.642578;
double WINDY_END_Z = 37.532829;

double WINDY_BEGIN_X = -2648.517334;
double WINDY_BEGIN_Y = 1498.584106;
double WINDY_BEGIN_Z = 118.599236;

bool should_reset = true;


// Mount Chiliad 247 Supermarket: x: 1722.844727  y:  6407.962891  z: 33.628269
// Franklin's street:              x:  -23.898239  y: -1439.207520  z: 30.273796


void perform_temp_vehicle_action(Ped player_ped, int vehicle, SharedRewardData* shared)
{
// Can't turn off, so just toggle to get some exploration in
//				WAIT(1000); // Don't wait here, we'll delay speed and spin metrics and get out of sync with steer / throttle from 
//				AI::TASK_VEHICLE_DRIVE_WANDER(player_ped, vehicle, 10.0, 786603);
//				featureVehShouldWander = true;
	// 1, 7, 8, 23
	// Actions:
	//'1 - brake
	//'3 - brake + reverse
	//'4 - turn left 90 + braking
	//'5 - turn right 90 + braking
	//'6 - brake strong (handbrake?) until time ends
	//'7 - turn left + accelerate
	//'7 - turn right + accelerate
	//'9 - weak acceleration
	//'10 - turn left + restore wheel pos to center in the end
	//'11 - turn right + restore wheel pos to center in the end
	//'13 - turn left + go reverse
	//'14 - turn left + go reverse
	//'16 - crash the game after like 2 seconds :)
	//'17 - keep actual state, game crashed after few tries
	//'18 - game crash
	//'19 - strong brake + turn left/right
	//'20 - weak brake + turn left then turn right
	//'21 - weak brake + turn right then turn left
	//'22 - brake + reverse
	//'23 - accelerate fast
	//'24 - brake
	//'25 - brake turning left then when almost stopping it turns left more
	//'26 - brake turning right then when almost stopping it turns right more
	//'27 - brake until car stop or until time ends
	//'28 - brake + strong reverse acceleration
	//'30 - performs a burnout (brake until stop + brake and accelerate)
	//'31 - accelerate in neutral
	//'32 - accelerate very strong
	
	int time = 100;

	if(! shared->should_perform_random_action)
	{
		// Do minimal action to disengage game AI
		AI::TASK_VEHICLE_TEMP_ACTION(player_ped, vehicle, 9, time);
	}
	else
	{
		vector<int> temp_actions({
			1, // brake
			7, // right + accelerate
			8, // left + accelerate
			23 // strong accelerate
		});
		int action = *select_randomly(temp_actions.begin(), temp_actions.end());
		AI::TASK_VEHICLE_TEMP_ACTION(player_ped, vehicle, action, time);
	}
}

void AgentCom()
{
	SharedRewardData* shared_reward_memory;
	InitializeSharedRewardMemory(&shared_reward_memory);
	set_status_text("initialized shared memory");
	logFile.open("native-trainver-log.txt");
	logFile << "start of log file" << endl;
	logFile.close();
	Player player_id;
	Ped player_ped;
	int vehicle = -1;
	int new_vehicle = -1;
	long long iter = 0;
	int control = 0;
	bool isGameDriving = featureVehShouldGameDrive;
	shared_reward_memory->should_game_drive = featureVehShouldGameDrive;
	bool hasGameDriven = false;

	while(shouldSelfDrive)
	{
		WAIT(10);

		if(iter % 100 == 0)
		{
			new_vehicle = PED::GET_VEHICLE_PED_IS_USING(PLAYER::PLAYER_PED_ID());		
		}

		if(iter % 500 == 0) // 5 seconds
		{
			logFile.close();
			logFile.open("native-trainver-log.txt", std::ios_base::app | std::ios_base::out);
		}

		iter++;
		
		if(new_vehicle == 0)
		{
			set_status_text("get a ride");
		}
		else if(new_vehicle != vehicle)
		{
			set_status_text("new vehicle detected " + std::to_string(new_vehicle));
			WAIT(3000); // Wait for player to enter
			vehicle = new_vehicle;
			should_reset = true;					
		}
		else if(should_reset || shared_reward_memory->should_reset_agent)
		{
//			set_status_text("SELF-DRIVING MODE    ...Is that you, Michael?");
			player_id = PLAYER::PLAYER_ID();
			player_ped = PLAYER::PLAYER_PED_ID();
			vehicle = PED::GET_VEHICLE_PED_IS_USING(player_ped);
			PLAYER::SET_EVERYONE_IGNORE_PLAYER(player_id, true);
			PLAYER::SET_POLICE_IGNORE_PLAYER(player_id, true);
			PLAYER::CLEAR_PLAYER_WANTED_LEVEL(player_id); // Never wanted

			// Put on seat belt
			const int PED_FLAG_CAN_FLY_THRU_WINDSCREEN = 32;
			if (PED::GET_PED_CONFIG_FLAG(player_ped, PED_FLAG_CAN_FLY_THRU_WINDSCREEN, TRUE))
			{
				PED::SET_PED_CONFIG_FLAG(player_ped, PED_FLAG_CAN_FLY_THRU_WINDSCREEN, FALSE);
			}

			// Invincible vehicle
			VEHICLE::SET_VEHICLE_TYRES_CAN_BURST(vehicle, FALSE);
			VEHICLE::SET_VEHICLE_WHEELS_CAN_BREAK(vehicle, FALSE);
			VEHICLE::SET_VEHICLE_HAS_STRONG_AXLES(vehicle, TRUE);

			VEHICLE::SET_VEHICLE_CAN_BE_VISIBLY_DAMAGED(vehicle, FALSE);
			ENTITY::SET_ENTITY_INVINCIBLE(vehicle, TRUE);
			ENTITY::SET_ENTITY_PROOFS(vehicle, 1, 1, 1, 1, 1, 1, 1, 1);	

			// Player invinsible
			PLAYER::SET_PLAYER_INVINCIBLE(player_id, TRUE);

			// Driving characteristics
			PED::SET_DRIVER_AGGRESSIVENESS(player_ped, 0.0);
			PED::SET_DRIVER_ABILITY(player_ped, 1.0);
			
			// Get change in heading so you can steer to match
			// Get change in velocity so you can steer / brake to match

			// ENTITY::GET_ENTITY_HEADING

			// ENTITY::GET_ENTITY_VELOCITY

			// Creates new vehicle and sets it in place. Now done by reloading saved game to reset world.
//			ENTITY::SET_ENTITY_COORDS_NO_OFFSET(PLAYER::PLAYER_PED_ID(), 2038.885986, 3754.751465, 31.947712, 0, 0, 1);
//			DWORD model = GAMEPLAY::GET_HASH_KEY((char *)"NINEF");
//			if (STREAMING::IS_MODEL_IN_CDIMAGE(model) && STREAMING::IS_MODEL_A_VEHICLE(model))
//			{
//				STREAMING::REQUEST_MODEL(model);				
//				while (!STREAMING::HAS_MODEL_LOADED(model)) WAIT(0);
//				Vector3 coords = ENTITY::GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS(PLAYER::PLAYER_PED_ID(), 0.0, 5.0, 0.0);
//				vehicle = VEHICLE::CREATE_VEHICLE(model, coords.x, coords.y, coords.z, 0.0, 1, 1);
//				VEHICLE::SET_VEHICLE_ON_GROUND_PROPERLY(vehicle);
//
//				ENTITY::SET_ENTITY_HEADING(vehicle, ENTITY::GET_ENTITY_HEADING(PLAYER::PLAYER_PED_ID()));
//				PED::SET_PED_INTO_VEHICLE(PLAYER::PLAYER_PED_ID(), vehicle, -1);
//
//				WAIT(0);
//				STREAMING::SET_MODEL_AS_NO_LONGER_NEEDED(model);
//				//ENTITY::SET_VEHICLE_AS_NO_LONGER_NEEDED(&vehicle);
//
//				ENTITY::SET_ENTITY_COORDS_NO_OFFSET(vehicle, 2038.885986, 3754.751465, 31.947712, 0, 0, 1);
//				ENTITY::SET_ENTITY_HEADING(vehicle, 209.866089);
//				
//			}
//			set_status_text("SELF-DRIVING MODE    ...Okay, let's go!");
			(*shared_reward_memory).should_reset_agent = false;
			should_reset = false;
		}
		else
		{
			auto horiz = CONTROLS::GET_CONTROL_VALUE(0, 9);
			auto vertical = CONTROLS::GET_CONTROL_VALUE(0, 8);
//			auto forward_test = ENTITY::GET_ENTITY_FORWARD_VECTOR(vehicle);
//
			string status = "";
//			status += "x: " + std::to_string(forward_test.x) + "\n" 
//					  "y: " + std::to_string(forward_test.y) + "\n" 
//					  "z: " + std::to_string(forward_test.z) + "\n";
//			set_status_text(status);

//			set_status_text("control: " + std::to_string(horiz) + "\n");
//			set_status_text("control: " + std::to_string(vertical) + "\n");

//			CONTROLS::ENABLE_ALL_CONTROL_ACTIONS(17);
//			CONTROLS::ENABLE_ALL_CONTROL_ACTIONS(18);
//			CONTROLS::ENABLE_ALL_CONTROL_ACTIONS(19);
//			CONTROLS::ENABLE_ALL_CONTROL_ACTIONS(20);
//			CONTROLS::ENABLE_ALL_CONTROL_ACTIONS(21);
//			CONTROLS::ENABLE_ALL_CONTROL_ACTIONS(22);
//			CONTROLS::ENABLE_ALL_CONTROL_ACTIONS(23);
//
//			CONTROLS::_0xE8A25867FBA3B05E(0, 9, 0); // Set control normal
//			CONTROLS::_0xE8A25867FBA3B05E(0, 8, 1); // Set control normal

			
			if( ! isGameDriving && (shared_reward_memory->should_game_drive || featureVehShouldGameDrive))
			{
				AI::TASK_VEHICLE_DRIVE_WANDER(player_ped, vehicle, 10.0, 786603);
				std::this_thread::sleep_for(std::chrono::milliseconds(500)); // Give time to take back control.
				shared_reward_memory->is_game_driving = true;
				isGameDriving = true;
				hasGameDriven = true;
				featureVehShouldGameDrive = true;
				shared_reward_memory->should_game_drive = true;
			}
			else if(isGameDriving && (!shared_reward_memory->should_game_drive || !featureVehShouldGameDrive))
			{
				shared_reward_memory->is_game_driving = false;
				isGameDriving = false;
				featureVehShouldGameDrive = false;
				shared_reward_memory->should_game_drive = false;
//				if(hasGameDriven)
//				{
					// We can't turn off wandering, but we can act randomly.
					perform_temp_vehicle_action(player_ped, vehicle, shared_reward_memory);

//				}
			}

			Vector3 vehCoords = ENTITY::GET_ENTITY_COORDS(vehicle, true);
			double heading = ENTITY::GET_ENTITY_HEADING(vehicle); // physics heading?
//			double speed = ENTITY::GET_ENTITY_SPEED(vehicle);
//			Vector3 forward = ENTITY::GET_ENTITY_FORWARD_VECTOR(vehicle);
			Vector3 speed = ENTITY::GET_ENTITY_SPEED_VECTOR(vehicle, true);
			auto spin = ENTITY::GET_ENTITY_ROTATION_VELOCITY(vehicle);

//			auto forward_vector = ENTITY::GET_ENTITY_FORWARD_VECTOR(vehicle);
//			if(shared_reward_memory->desired_spin > -128)
//			{
////				ENTITY::SET_ENTITYa_ROTATIONAL_VELOCITY(vehicle, shared_reward_memory->desired_heading);
//				logFile << "spin:\t" << spin.z << "\tdesired_direction:\t" << shared_reward_memory->desired_direction 
//					<< "\tdesired_spin:\t" << shared_reward_memory->desired_spin;
//				double magnitude = shared_reward_memory->desired_speed / speed;
////				ENTITY::SET_ENTITY_VELOCITY(vehicle, forward_vector.x * magnitude, forward_vector.y * magnitude, forward_vector.z * magnitude);
//				logFile << "\tspeed:\t" << speed.y << "\tdesired_speed:\t" << shared_reward_memory->desired_speed;
//				logFile << "\tdesired_speed_change:\t" << shared_reward_memory->desired_speed_change << endl;
//			}

			bool point_on_road = PATHFIND::IS_POINT_ON_ROAD(vehCoords.x, vehCoords.y, vehCoords.z, vehicle);
			if(point_on_road)
			{
				status += "on road: yes\n ";
			}
			else
			{
				status += "on road: no\n ";
			}
			
			double distance = sqrt( 
				  pow(WINDY_BEGIN_X - double(vehCoords.x), 2) + 
				  pow(WINDY_BEGIN_Y - double(vehCoords.y), 2) + 
				  pow(WINDY_BEGIN_Z - double(vehCoords.z), 2));

			// Doesn't return more than 100000.
//			float distance = PATHFIND::CALCULATE_TRAVEL_DISTANCE_BETWEEN_POINTS(
//				vehCoords.x, vehCoords.y, vehCoords.z,
//				CHILIAD_X, CHILIAD_Y, CHILIAD_Z);

//			status += "distance: " + std::to_string(distance);

			status += "Game driving: " + std::to_string(isGameDriving) + "\n"
//					  "should wander: " + std::to_string(shared_reward_memory->should_wander) + "\n"
//					  "wanderFeature: " + std::to_string(featureVehShouldWander) + "\n"
//					  "should reset: " + std::to_string((*shared_reward_memory).should_reset_agent) + "\n"
					  "sx: "  + std::to_string(speed.x)   + "\n" 
					  "sy: "  + std::to_string(speed.y)   + "\n" 
					  "sz: "  + std::to_string(speed.z)   + "\n"
//					  "h: "  + std::to_string(heading)	     + "\n"
					  "rz: " + std::to_string(spin.z)        + "\n"
			;

			if(featureVehShowStats)
			{
				set_status_text(status);				
			}


			(*shared_reward_memory).distance   = distance;
			(*shared_reward_memory).on_road    = point_on_road;
			(*shared_reward_memory).spin       = spin.z;
			(*shared_reward_memory).heading    = heading;
			(*shared_reward_memory).speed      = speed.y;
		}
	}
}

//class MySwapChain : IDXGISwapChain
//{
//public:
//	virtual ~MySwapChain(){}
//
//	HRESULT STDMETHODCALLTYPE PresentHook(UINT SyncInterval, UINT Flags)
//	{
//		HRESULT hr = reinterpret_cast<
//			HRESULT (STDMETHODCALLTYPE *)(IDXGISwapChain *pthis, UINT a, UINT b)
//		>(origPresent)(this, SyncInterval, Flags);
//
//		if(checkHooked)
//		{
//			logFile << "inside first hooked present\n";
//			checkHooked = false;
//		}
//
//		return hr;
//	}
//};
//
//// For hooking ID3D11Device virtual methods called with "this"
//class MyDevice : ID3D11Device
//{
//public:
//	virtual ~MyDevice(){}
//
//	HRESULT STDMETHODCALLTYPE CreateDepthStencilViewHook(ID3D11Resource *pResource,
//														 const D3D11_DEPTH_STENCIL_VIEW_DESC *pDesc,
//														 ID3D11DepthStencilView **ppDepthStencilView)
//	{
//		HRESULT hr = reinterpret_cast<HRESULT (STDMETHODCALLTYPE *)(
//			ID3D11Device *pthis,
//			ID3D11Resource *res,
//			const D3D11_DEPTH_STENCIL_VIEW_DESC *desc, 
//			ID3D11DepthStencilView **dsv)
//		>(origCreateDepth)(this, pResource, pDesc, ppDepthStencilView);
//
//		if(checkDepthHooked)
//		{
////			savedDepthResource = pResource; // Save for copying later
//			checkDepthHooked = false;
//		}
//
//		return hr;
//	}
//};

//void setupCreateDepthStencilViewHook(ID3D11Device *device)
//{
//	logFile << "locating vtable to hook create depth\n";
//	unsigned vtableOffset = 10U;  // CreateDepthStencilView is index 10 in c interface
//	unsigned long * vtablePtr = *reinterpret_cast<unsigned long**>(device);
//	origCreateDepth = vtablePtr[vtableOffset];
//	CreateDepthStencilViewFuncType* vFunc = reinterpret_cast<CreateDepthStencilViewFuncType*>(vtablePtr);
//	HANDLE process = ::GetCurrentProcess();
//	DWORD protection = PAGE_READWRITE;
//	DWORD oldProtection;
//	logFile << "found vtable to hook create depth\n";
//	logFile << "removing write protection to hook create depth\n";
//	if ( ! ::VirtualProtectEx( process, &vFunc[vtableOffset], sizeof(int), protection, &oldProtection ) )
//	{
//		logFile << "could not remove vtable write protection for method\n";
////		throw std::exception( "could not remove vtable write protection for method" );
//	} 
//	else {
//		logFile << "successfully removed write protection to hook create depth\n";
//		logFile << "attempting to hook create depth\n";
//		try
//		{
////			vFunc[vtableOffset] = reinterpret_cast<CreateDepthStencilViewFuncType>(&MyDevice::CreateDepthStencilViewHook);
//			logFile << "success in hooking create depth\n";
//		}
//		catch(...) {
//			logFile << "failed to hook create depth\n";
//			logFile.close();
//			return;
//		}
//
//		if ( ! ::VirtualProtectEx( process, &vFunc[vtableOffset], sizeof(int), oldProtection, &oldProtection ) )
//		{
//			logFile << "could not remove vtable write protection for method" ;
//		}
////		else {
////			if ( ! ::VirtualProtectEx( process, &origCreateDepth, sizeof(int), protection, &oldProtection ) )
////			{
////				// TODO: Determine if needed when process is separate, since only necessary in mock DXGI proj.
////				throw std::exception( "could not remove protection that prevents calling original method" );
////			}
////		}
////		logFile << "success in reprotecting create depth";
//		logFile.close();
//	}
//}
//
//void setupPresentHook(IDXGISwapChain* swapchain)
//{
//	unsigned vtableOffset = 8U;
//	unsigned long * vtablePtr = *reinterpret_cast<unsigned long**>(swapchain);
//	origPresent = vtablePtr[vtableOffset];
//	PresentFuncType* vFunc = reinterpret_cast<PresentFuncType*>(vtablePtr);
//	HANDLE process = ::GetCurrentProcess();
//	DWORD protection = PAGE_READWRITE;
//	DWORD oldProtection;
//	if ( ! ::VirtualProtectEx( process, &vFunc[vtableOffset], sizeof(int), protection, &oldProtection ) )
//	{
//		throw std::exception( "could not remove vtable write protection for present method" );
//	} 
//	else {
////		vFunc[vtableOffset] = reinterpret_cast<PresentFuncType>(&MySwapChain::PresentHook);
//		if ( ! ::VirtualProtectEx( process, &vFunc[vtableOffset], sizeof(int), oldProtection, &oldProtection ) )
//		{
//			throw std::exception( "could not set back to original protection" );
//		} 
//		else {
//			if ( ! ::VirtualProtectEx( process, &origPresent, sizeof(int), protection, &oldProtection ) )
//			{
//				// TODO: Determine if needed when process is separate, since only necessary in mock DXGI proj.
//				throw std::exception( "could not remove protection that prevents calling original present" );
//			}
//		}
//	}
//}