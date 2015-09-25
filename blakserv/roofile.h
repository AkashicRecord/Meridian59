// Meridian 59, Copyright 1994-2012 Andrew Kirmse and Chris Kirmse.
// All rights reserved.
//
// This software is distributed under a license that is described in
// the LICENSE file that accompanies it.
//
// Meridian is a registered trademark.
/*
 * roofile.h
 *
 */

#ifndef _ROOFILE_H
#define _ROOFILE_H

#include "geometry.h"

#pragma region Macros
/**************************************************************************************************************/
/*                                           MACROS                                                           */
/**************************************************************************************************************/
#define DEBUGLOS            0                  // switch to 1 to enable debug output for BSP LOS
#define DEBUGMOVE           0                  // switch to 1 to enable debug output for BSP MOVES
#define ROO_VERSION         14                 // required roo fileformat version (V14 = floatingpoint)
#define ROO_SIGNATURE       0xB14F4F52         // signature of ROO files (first 4 bytes)
#define OBJECTHEIGHTROO     768                // estimated object height (used in LOS and MOVE calcs)
#define ROOFINENESS         1024.0f            // fineness used in ROO files
#define FINENESSKODTOROO(x) ((x) * 16.0f)      // scales a value from KOD fineness to ROO fineness
#define FINENESSROOTOKOD(x) ((x) * 0.0625f)    // scales a value from ROO fineness to KOD fineness

#define MAXSTEPHEIGHT       ((float)(24 << 4))                     // (from clientd3d/move.c)
#define PLAYERWIDTH         (31.0f * (float)KODFINENESS * 0.25f)   // (from clientd3d/game.c)
#define WALLMINDISTANCE     (PLAYERWIDTH / 2.0f)                   // (from clientd3d/game.c)
#define WALLMINDISTANCE2    (WALLMINDISTANCE * WALLMINDISTANCE)    // (from clientd3d/game.c)

// converts grid coordinates
// input: 1-based value of big row (or col), 0-based value of fine-row (or col)
// output: 0-based value in ROO fineness
#define GRIDCOORDTOROO(big, fine) \
   (FINENESSKODTOROO((float)(big - 1) * (float)KODFINENESS + (float)fine))

// converts a floatingpoint-value into KOD integer (boxes into MAX/MIN KOD INT)
#define FLOATTOKODINT(x) \
   (((x) > (float)MAX_KOD_INT) ? MAX_KOD_INT : (((x) < (float)-MIN_KOD_INT) ? -MIN_KOD_INT : (int)x))

#pragma endregion

#pragma region Structs
/**************************************************************************************************************/
/*                                          STRUCTS                                                           */
/**************************************************************************************************************/
typedef struct BoundingBox2D
{
   V2 Min;
   V2 Max;
} BoundingBox2D;

typedef struct Side
{
   unsigned short ServerID;
   unsigned short TextureMiddle;
   unsigned short TextureUpper;
   unsigned short TextureLower;
   unsigned int   Flags;
   unsigned short TextureMiddleOrig;
   unsigned short TextureUpperOrig;
   unsigned short TextureLowerOrig;
} Side;

typedef struct SlopeInfo
{
   float A;
   float B;
   float C;
   float D;
} SlopeInfo;

typedef struct Sector
{
   unsigned short ServerID;
   unsigned short FloorTexture;
   unsigned short CeilingTexture;
   float          FloorHeight;
   float          CeilingHeight;
   unsigned int   Flags;
   SlopeInfo*     SlopeInfoFloor;
   SlopeInfo*     SlopeInfoCeiling;
   unsigned short FloorTextureOrig;
   unsigned short CeilingTextureOrig;
} Sector;

typedef struct Wall
{
   unsigned short Num;
   unsigned short NextWallInPlaneNum;
   unsigned short RightSideNum;
   unsigned short LeftSideNum;
   V2             P1;
   V2             P2;
   unsigned short RightSectorNum;
   unsigned short LeftSectorNum;
   Sector*        RightSector;
   Sector*        LeftSector;
   Side*          RightSide;
   Side*          LeftSide;
   Wall*          NextWallInPlane;
} Wall;

typedef enum BspNodeType
{
   BspInternalType = 1,
   BspLeafType = 2
} BspNodeType;

typedef struct BspInternal
{
   float           A;
   float           B;
   float           C;
   unsigned short  RightChildNum;
   unsigned short  LeftChildNum;
   unsigned short  FirstWallNum;
   struct BspNode* RightChild;
   struct BspNode* LeftChild;
   Wall*           FirstWall;
} BspInternal;

typedef struct BspLeaf
{
   unsigned short SectorNum;
   unsigned short PointsCount;
   V3*            PointsFloor;
   V3*            PointsCeiling;
   Sector*        Sector;
} BspLeaf;

typedef struct BspNode
{
   BspNodeType    Type;
   BoundingBox2D  BoundingBox;

   union
   {
      BspInternal internal;
      BspLeaf     leaf;
   } u;

} BspNode;

typedef struct room_type
{
   int roomdata_id; 
   int resource_id;
   
   int rows;             /* Size of room in grid squares */
   int cols;
   int rowshighres;      /* Size of room in highres grid squares */
   int colshighres;

   int security;         /* Security number, to ensure that client loads the correct roo file */
   
   BoundingBox2D  ThingsBox;

   BspNode*       TreeNodes;
   unsigned short TreeNodesCount;
   Wall*          Walls;
   unsigned short WallsCount;
   Side*          Sides;
   unsigned short SidesCount;
   Sector*        Sectors;
   unsigned short SectorsCount; 
} room_type;
#pragma endregion

#pragma region Methods
/**************************************************************************************************************/
/*                                          METHODS                                                           */
/**************************************************************************************************************/
float BSPGetHeight(room_type* Room, V2* P, bool Floor, bool WithDepth);
bool  BSPCanMoveInRoom(room_type* Room, V2* S, V2* E);
bool  BSPLineOfSight(room_type* Room, V3* S, V3* E);
void  BSPChangeTexture(room_type* Room, unsigned int ServerID, unsigned short NewTexture, unsigned int Flags);
void  BSPMoveSector(room_type* Room, unsigned int ServerID, bool Floor, float Height, float Speed);
bool  BSPLoadRoom(char *fname, room_type *room);
void  BSPFreeRoom(room_type *room);
#pragma endregion

#endif
