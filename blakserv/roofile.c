// Meridian 59, Copyright 1994-2012 Andrew Kirmse and Chris Kirmse.
// All rights reserved.
//
// This software is distributed under a license that is described in
// the LICENSE file that accompanies it.
//
// Meridian is a registered trademark.
/*
 * roofile.c
 * 
 
 Server-side implementation of a ROO file.
 Loads the BSP tree like the client does and provides
 BSP queries on the tree, such as LineOfSightBSP or CanMoveInRoomBSP

 */

#include "blakserv.h"

#pragma region Macros
/*****************************************************************************************
********* macro functions ****************************************************************
*****************************************************************************************/
// distance from a point (b) to a BspInternal (a)
#define DISTANCETOSPLITTERSIGNED(a,b)	((a)->A * (b)->X + (a)->B * (b)->Y + (a)->C)

// floorheight of a point (b) in a sector (a)
#define SECTORHEIGHTFLOOR(a, b)	\
   ((!(a)->SlopeInfoFloor) ? (a)->FloorHeight : \
      (-(a)->SlopeInfoFloor->A * (b)->X \
       -(a)->SlopeInfoFloor->B * (b)->Y \
       -(a)->SlopeInfoFloor->D) / (a)->SlopeInfoFloor->C)

// ceilingheight of a point (b) in a sector (a)
#define SECTORHEIGHTCEILING(a, b)	\
   ((!(a)->SlopeInfoCeiling) ? (a)->CeilingHeight : \
      (-(a)->SlopeInfoCeiling->A * (b)->X \
       -(a)->SlopeInfoCeiling->B * (b)->Y \
       -(a)->SlopeInfoCeiling->D) / (a)->SlopeInfoCeiling->C)

/*****************************************************************************************
********* from clientd3d/draw3d.c ********************************************************
*****************************************************************************************/
#define DEPTHMODIFY0    0.0f
#define DEPTHMODIFY1    (ROOFINENESS / 5.0f)
#define DEPTHMODIFY2    (2.0f * ROOFINENESS / 5.0f)
#define DEPTHMODIFY3    (3.0f * ROOFINENESS / 5.0f)

/*****************************************************************************************
********* from clientd3d/bsp.h ***********************************************************
*****************************************************************************************/
#define SF_DEPTH0          0x00000000      // Sector has default (0) depth
#define SF_DEPTH1          0x00000001      // Sector has shallow depth
#define SF_DEPTH2          0x00000002      // Sector has deep depth
#define SF_DEPTH3          0x00000003      // Sector has very deep depth
#define SF_MASK_DEPTH      0x00000003      // Gets depth type from flags
#define SF_SLOPED_FLOOR    0x00000400      // Sector has sloped floor
#define SF_SLOPED_CEILING  0x00000800      // Sector has sloped ceiling
#define WF_TRANSPARENT     0x00000002      // normal wall has some transparency
#define WF_PASSABLE        0x00000004      // wall can be walked through
#define WF_NOLOOKTHROUGH   0x00000020      // bitmap can't be seen through even though it's transparent
#pragma endregion

#pragma region Internal
/**************************************************************************************************************/
/*                                            INTERNAL                                                        */
/*                                   These are not defined in header                                          */
/**************************************************************************************************************/

__inline float GetSectorHeightFloorWithDepth(Sector* Sector, V2* P)
{
   float height = SECTORHEIGHTFLOOR(Sector, P);
   unsigned int depthtype = Sector->Flags & SF_MASK_DEPTH;

   if (depthtype == SF_DEPTH0)
      return (height - DEPTHMODIFY0);

   if (depthtype == SF_DEPTH1)
      return (height - DEPTHMODIFY1);

   if (depthtype == SF_DEPTH2)
      return (height - DEPTHMODIFY2);

   if (depthtype == SF_DEPTH3)
      return (height - DEPTHMODIFY3);

   return height;
}

void BSPUpdateLeafHeights(room_type* Room, Sector* Sector, bool Floor)
{
   for (int i = 0; i < Room->TreeNodesCount; i++)
   {
      BspNode* node = &Room->TreeNodes[i];

      if (node->Type != BspLeafType || !node->u.leaf.Sector || node->u.leaf.Sector != Sector)
         continue;

      for (int j = 0; j < node->u.leaf.PointsCount; j++)
      {
         V2 p = { node->u.leaf.PointsFloor[j].X, node->u.leaf.PointsFloor[j].Y };

         if (Floor)
            node->u.leaf.PointsFloor[j].Z = SECTORHEIGHTFLOOR(node->u.leaf.Sector, &p);

         else
            node->u.leaf.PointsCeiling[j].Z = SECTORHEIGHTCEILING(node->u.leaf.Sector, &p);
      }
   }
}

float BSPGetHeightTree(BspNode* Node, V2* P, bool Floor, bool WithDepth)
{
   if (!Node)
      return (float)-MIN_KOD_INT;

   // reached a leaf, return its floor or ceiling height
   if (Node->Type == BspLeafType && Node->u.leaf.Sector)
   {
      if (Floor)
      {
         return (WithDepth) ? GetSectorHeightFloorWithDepth(Node->u.leaf.Sector, P) :
            SECTORHEIGHTFLOOR(Node->u.leaf.Sector, P);
      }
      else
         return SECTORHEIGHTCEILING(Node->u.leaf.Sector, P);
   }

   // still internal node, climb down only one subtree
   else if (Node->Type == BspInternalType)
   {
      return (DISTANCETOSPLITTERSIGNED(&Node->u.internal, P) >= 0.0f) ?
         BSPGetHeightTree(Node->u.internal.RightChild, P, Floor, WithDepth) :
         BSPGetHeightTree(Node->u.internal.LeftChild, P, Floor, WithDepth);
   }

   return (float)-MIN_KOD_INT;
}

bool BSPLineOfSightTree(BspNode* Node, V3* S, V3* E)
{
	if (!Node)
		return true;

	/****************************************************************/

	// reached a leaf
	if (Node->Type == BspLeafType)
	{
		// no collisions with leafs without sectors
		if (!Node->u.leaf.Sector)
			return true;

		// floors and ceilings don't have backsides.
		// therefore a floor can only collide if
		// the start height is bigger than end height
		// and for ceiling the other way round.
		if (S->Z > E->Z && Node->u.leaf.Sector->FloorTexture > 0)
		{
			for (int i = 0; i < Node->u.leaf.PointsCount - 2; i++)
			{
				bool blocked = IntersectLineTriangle(
					&Node->u.leaf.PointsFloor[i + 2],
					&Node->u.leaf.PointsFloor[i + 1],
					&Node->u.leaf.PointsFloor[0], S, E);

				// blocked by floor
				if (blocked)
				{
#if DEBUGLOS
					dprintf("BLOCK - FLOOR");
#endif
					return false;
				}
			}
		}

		else if (S->Z < E->Z && Node->u.leaf.Sector->CeilingTexture > 0)
		{
			for (int i = 0; i < Node->u.leaf.PointsCount - 2; i++)
			{
				bool blocked = IntersectLineTriangle(
					&Node->u.leaf.PointsCeiling[i + 2],
					&Node->u.leaf.PointsCeiling[i + 1],
					&Node->u.leaf.PointsCeiling[0], S, E);

				// blocked by ceiling
				if (blocked)
				{
#if DEBUGLOS
					dprintf("BLOCK - CEILING");
#endif
					return false;
				}
			}
		}

		// not blocked by this leaf
		return true;
	}

	/****************************************************************/

	// expecting anything else/below to be a splitter
	if (Node->Type != BspInternalType)
		return true;

	// get signed distances to both endpoints of ray
	float distS = DISTANCETOSPLITTERSIGNED(&Node->u.internal, S);
	float distE = DISTANCETOSPLITTERSIGNED(&Node->u.internal, E);

	/****************************************************************/

	// both endpoints on positive (right) side
	// --> climb down only right subtree
	if (distS > EPSILON && distE > EPSILON)
		return BSPLineOfSightTree(Node->u.internal.RightChild, S, E);

	// both endpoints on negative (left) side
	// --> climb down only left subtree
	else if (distS < -EPSILON && distE < -EPSILON)
		return BSPLineOfSightTree(Node->u.internal.LeftChild, S, E);

	// endpoints are on different sides or one/both on infinite line
	// --> check walls of splitter first and then possibly climb down both
	else
	{
		// loop through walls in this splitter and check for collision
		Wall* wall = Node->u.internal.FirstWall;
		while (wall)
		{
			// must have at least a sector on one side of the wall
			// otherwise skip this wall
			if (!wall->RightSector && !wall->LeftSector)
			{
				wall = wall->NextWallInPlane;
				continue;
			}

			// pick side ray is coming from
			Side* side = (distS > 0.0f) ? wall->RightSide : wall->LeftSide;

			// no collision with unset sides
			if (!side)
			{
				wall = wall->NextWallInPlane;
				continue;
			}

			// get 2d line equation coefficients for infinite line through S and E
			float a1, b1, c1;
			a1 = E->Y - S->Y;
			b1 = S->X - E->X;
			c1 = a1 * S->X + b1 * S->Y;

			// get 2d line equation coefficients for infinite line through P1 and P2
			// NOTE: This should be using BspInternal A,B,C coefficients
			float a2, b2, c2;
			a2 = wall->P2.Y - wall->P1.Y;
			b2 = wall->P1.X - wall->P2.X;
			c2 = a2 * wall->P1.X + b2 * wall->P1.Y;

			float det = a1*b2 - a2*b1;

			// parallel (or identical) lines
			if (ISZERO(det))
			{
				wall = wall->NextWallInPlane;
				continue;
			}

			// intersection point of infinite lines
			V2 q;
			q.X = (b2*c1 - b1*c2) / det;
			q.Y = (a1*c2 - a2*c1) / det;

			//dprintf("intersect: %f %f \t p1.x:%f p1.y:%f p2.x:%f p2.y:%f \n", q.X, q.Y, wall->P1.X, wall->P1.Y, wall->P2.X, wall->P2.Y);

			// infinite intersection point must be in BOTH
			// finite segments boundingboxes, otherwise no intersect
			if (!ISINBOX(S, E, &q) || !ISINBOX(&wall->P1, &wall->P2, &q))
			{
				wall = wall->NextWallInPlane;
				continue;
			}

			// vector from (S)tart to (E)nd
			V3 se;
			V3SUB(&se, E, S);

			// find rayheight of (S->E) at intersection point
			float lambda = 1.0f;
			if (!ISZERO(se.X))
				lambda = (q.X - S->X) / se.X;

			else if (!ISZERO(se.Y))
				lambda = (q.Y - S->Y) / se.Y;

			float rayheight = S->Z + lambda * se.Z;

			// get heights of right and left floor/ceiling
			float hFloorRight = (wall->RightSector) ?
				SECTORHEIGHTFLOOR(wall->RightSector, &q) :
				SECTORHEIGHTFLOOR(wall->LeftSector, &q);

			float hFloorLeft = (wall->LeftSector) ?
				SECTORHEIGHTFLOOR(wall->LeftSector, &q) :
				SECTORHEIGHTFLOOR(wall->RightSector, &q);

			float hCeilingRight = (wall->RightSector) ?
				SECTORHEIGHTCEILING(wall->RightSector, &q) :
				SECTORHEIGHTCEILING(wall->LeftSector, &q);

			float hCeilingLeft = (wall->LeftSector) ?
				SECTORHEIGHTCEILING(wall->LeftSector, &q) :
				SECTORHEIGHTCEILING(wall->RightSector, &q);

			// build all 4 possible heights (h0 lowest)
			float h3 = fmax(hCeilingRight, hCeilingLeft);
			float h2 = fmax(fmin(hCeilingRight, hCeilingLeft), fmax(hFloorRight, hFloorLeft));
			float h1 = fmin(fmin(hCeilingRight, hCeilingLeft), fmax(hFloorRight, hFloorLeft));
			float h0 = fmin(hFloorRight, hFloorLeft);

			// above maximum or below minimum
			if (rayheight > h3 || rayheight < h0)
			{
				wall = wall->NextWallInPlane;
				continue;
			}

			// ray intersects middle wall texture
			if (rayheight <= h2 && rayheight >= h1 && side->TextureMiddle > 0)
			{
				// get some flags from the side we're coming from
				// these are applied only to the 'main' = 'middle' texture
				bool isNoLookThrough = ((side->Flags & WF_NOLOOKTHROUGH) == WF_NOLOOKTHROUGH);
				bool isTransparent   = ((side->Flags & WF_TRANSPARENT) == WF_TRANSPARENT);

				// 'transparent' middle textures block only
				// if they are set so by 'no-look-through'
				if (!isTransparent || (isTransparent && isNoLookThrough))
				{
#if DEBUGLOS
					dprintf("WALL %i - MID - (%f/%f/%f)", wall->Num, q.X, q.Y, rayheight);
#endif
					return false;
				}
			}

			// ray intersects upper wall texture
			if (rayheight <= h3 && rayheight >= h2 && side->TextureUpper > 0)
			{
#if DEBUGLOS
				dprintf("WALL %i - UP - (%f/%f/%f)", wall->Num, q.X, q.Y, rayheight);
#endif
				return false;
			}

			// ray intersects lower wall texture
			if (rayheight <= h1 && rayheight >= h0 && side->TextureLower > 0)
			{
#if DEBUGLOS
				dprintf("WALL %i - LOW - (%f/%f/%f)", wall->Num, q.X, q.Y, rayheight);
#endif
				return false;
			}

			// next wall for next loop
			wall = wall->NextWallInPlane;
		}

		/****************************************************************/

		// try right subtree first
		bool retval = BSPLineOfSightTree(Node->u.internal.RightChild, S, E);

		// found a collision there? return it
		if (!retval)
			return retval;

		// otherwise try left subtree
		return BSPLineOfSightTree(Node->u.internal.LeftChild, S, E);
	}
}

bool BSPCanMoveInRoomTree(BspNode* Node, V2* S, V2* E)
{
	// reached a leaf or nullchild, movements not blocked by leafs
	if (!Node || Node->Type != BspInternalType)
		return true;

	/****************************************************************/

	// get signed distances from splitter to both endpoints of move
	float distS = DISTANCETOSPLITTERSIGNED(&Node->u.internal, S);
	float distE = DISTANCETOSPLITTERSIGNED(&Node->u.internal, E);

	/****************************************************************/

	// both endpoints far away enough on positive (right) side
	// --> climb down only right subtree
	if (distS > WALLMINDISTANCE && distE > WALLMINDISTANCE)
		return BSPCanMoveInRoomTree(Node->u.internal.RightChild, S, E);

	// both endpoints far away enough on negative (left) side
	// --> climb down only left subtree
	else if (distS < -WALLMINDISTANCE && distE < -WALLMINDISTANCE)
		return BSPCanMoveInRoomTree(Node->u.internal.LeftChild, S, E);

	// endpoints are on different sides, one/both on infinite line or potentially too close
	// --> check walls of splitter first and then possibly climb down both subtrees
	else
	{
		// loop through walls in this splitter
		Wall* wall = Node->u.internal.FirstWall;
		while (wall)
		{
			// these will be filled by two cases below
			V2 q;
			Side* sideS;
			Sector* sectorS;
			Side* sideE;
			Sector* sectorE;

			// CASE 1) The move line actually crosses this infinite splitter.
			// This case handles long movelines where S and E can be far away from each other and
			// just checking the distance of E to the line would fail.
			// q contains the intersection point
			if ((distS > 0.0f && distE < 0.0f) ||
				(distS < 0.0f && distE > 0.0f))
			{
				// get 2d line equation coefficients for infinite line through S and E
				float a1, b1, c1;
				a1 = E->Y - S->Y;
				b1 = S->X - E->X;
				c1 = a1 * S->X + b1 * S->Y;

				// get 2d line equation coefficients for infinite line through P1 and P2
				// NOTE: This should be using BspInternal A,B,C coefficients
				float a2, b2, c2;
				a2 = wall->P2.Y - wall->P1.Y;
				b2 = wall->P1.X - wall->P2.X;
				c2 = a2 * wall->P1.X + b2 * wall->P1.Y;

				float det = a1*b2 - a2*b1;

				// parallel (or identical) lines
				// should not happen here but is important for div by 0
				if (ISZERO(det))
				{
					wall = wall->NextWallInPlane;
					continue;
				}

				// intersection point of infinite lines				
				q.X = (b2*c1 - b1*c2) / det;
				q.Y = (a1*c2 - a2*c1) / det;

				//dprintf("intersect: %f %f \t p1.x:%f p1.y:%f p2.x:%f p2.y:%f \n", q.X, q.Y, wall->P1.X, wall->P1.Y, wall->P2.X, wall->P2.Y);

				// infinite intersection point must be in BOTH
				// finite segments boundingboxes, otherwise no intersect
				if (!ISINBOX(S, E, &q) || !ISINBOX(&wall->P1, &wall->P2, &q))
				{
					wall = wall->NextWallInPlane;
					continue;
				}

				// set from and to sector / side
				if (distS > 0.0f)
				{
					sideS = wall->RightSide;
					sectorS = wall->RightSector;
				}
				else
				{
					sideS = wall->LeftSide;
					sectorS = wall->LeftSector;
				}

				if (distE > 0.0f)
				{
					sideE = wall->RightSide;
					sectorE = wall->RightSector;
				}
				else
				{
					sideE = wall->LeftSide;
					sectorE = wall->LeftSector;
				}
			}

			// CASE 2) The move line does not cross the infinite splitter, both move endpoints are on the same side.
			// This handles short moves where walls are not intersected, but the endpoint may be too close
			// q will store the too-close endpoint
			else
			{
				// allow getting "away" from wall
				// even in case both endpoints would be too close
				if (distE > distS)
				{
					wall = wall->NextWallInPlane;
					continue;
				}

				// get min. squared distance from move endpoint to line segment
				float dist2 = MinSquaredDistanceToLineSegment(E, &wall->P1, &wall->P2);

				// skip if far enough away
				if (dist2 > WALLMINDISTANCE2)
				{
					wall = wall->NextWallInPlane;
					continue;
				}

				q.X = E->X;
				q.Y = E->Y;

				// set from and to sector / side
				// for case 2 (too close) these are based on (S),
				// and (E) is assumed to be on the other side.
				if (distS >= 0.0f)
				{
					sideS = wall->RightSide;
					sectorS = wall->RightSector;
					sideE = wall->LeftSide;
					sectorE = wall->LeftSector;
				}
				else
				{
					sideS = wall->LeftSide;
					sectorS = wall->LeftSector;
					sideE = wall->RightSide;
					sectorE = wall->RightSector;
				}
			}

			/****************************************/
			/*   From here on both cases together   */
			/****************************************/

			// block moves with end outside
			if (!sectorE || !sideE)
			{
#if DEBUGMOVE
				dprintf("MOVEBLOCK (END OUTSIDE): W:%i", wall->Num);
#endif
				return false;
			}

			// don't block moves with start outside (and end inside, see above)
			if (!sectorS || !sideS)
			{
#if DEBUGMOVE
				dprintf("MOVEALLOW (START OUT, END IN): W:%i", wall->Num);
#endif
				wall = wall->NextWallInPlane;
				continue;
			}

			// sides which have no passable flag set always block
			if (!((sideS->Flags & WF_PASSABLE) == WF_PASSABLE))
				return false;

			// get heights
			float hFloorS = GetSectorHeightFloorWithDepth(sectorS, &q);
			float hFloorE = GetSectorHeightFloorWithDepth(sectorE, &q);
			float hCeilingS = SECTORHEIGHTCEILING(sectorS, &q);
			float hCeilingE = SECTORHEIGHTCEILING(sectorE, &q);

			// check stepheight (this also requires a lower texture set)
			if (sideS->TextureLower > 0 && (hFloorE - hFloorS > MAXSTEPHEIGHT))
			{
#if DEBUGMOVE
				dprintf("MOVEBLOCK (STEPHEIGHT): W:%i HFS:%1.2f HFE:%1.2f", wall->Num, hFloorS, hFloorE);
#endif
				return false;
			}

			// check ceilingheight (this also requires an upper texture set)
			if (sideS->TextureUpper > 0 && (hCeilingE - hFloorS < OBJECTHEIGHTROO))
			{
#if DEBUGMOVE
				dprintf("MOVEBLOCK (UPWALL): W:%i HFS:%1.2f HCE:%1.2f", wall->Num, hFloorS, hCeilingE);
#endif
				return false;
			}

			// check endsector height
			if (hCeilingE - hFloorE < OBJECTHEIGHTROO)
			{
#if DEBUGMOVE
				dprintf("MOVEBLOCK (SECTHEIGHT): W:%i HFE:%1.2f HCE:%1.2f", wall->Num, hFloorE, hCeilingE);
#endif
				return false;
			}

			// next wall for next loop
			wall = wall->NextWallInPlane;
		}

		/****************************************************************/

		// try right subtree first
		bool retval = BSPCanMoveInRoomTree(Node->u.internal.RightChild, S, E);

		// found a collision there? return it
		if (!retval)
			return retval;

		// otherwise try left subtree
		return BSPCanMoveInRoomTree(Node->u.internal.LeftChild, S, E);
	}
}
#pragma endregion

#pragma region Public
/**************************************************************************************************************/
/*                                            PUBLIC                                                          */
/*                     These are defined in header and can be called from outside                             */
/**************************************************************************************************************/

/*********************************************************************************************/
/* BSPGetHeight:  Returns the floor or ceiling height in a room for a given location.        */
/*                Returns -MIN_KOD_INT (-134217728) for a location outside of the map.       */
/*********************************************************************************************/
float BSPGetHeight(room_type* Room, V2* P, bool Floor, bool WithDepth)
{
   if (!Room || Room->TreeNodesCount == 0 || !P)
      return 0.0f;

   return BSPGetHeightTree(&Room->TreeNodes[0], P, Floor, WithDepth);
}

/*********************************************************************************************/
/* BSPLineOfSight:  Checks if location E(nd) can be seen from location S(tart)               */
/*********************************************************************************************/
bool BSPLineOfSight(room_type* Room, V3* S, V3* E)
{
   if (!Room || Room->TreeNodesCount == 0 || !S || !E)
      return false;

   return BSPLineOfSightTree(&Room->TreeNodes[0], S, E);
}

/*********************************************************************************************/
/* BSPCanMoveInRoom:  Checks if you can walk a straight line from (S)tart to (E)nd           */
/*********************************************************************************************/
bool BSPCanMoveInRoom(room_type* Room, V2* S, V2* E)
{
   if (!Room || Room->TreeNodesCount == 0 || !S || !E)
      return false;

   // allow move to same location
   if (ISZERO(S->X - E->X) && ISZERO(S->Y - E->Y))
   {
#if DEBUGMOVE
      dprintf("MOVEALLOW (START=END)");
#endif
      return true;
   }

   return BSPCanMoveInRoomTree(&Room->TreeNodes[0], S, E);
}

/*********************************************************************************************/
/* BSPChangeTexture: Sets textures of sides and/or sectors to given NewTexture num based on Flags
/*********************************************************************************************/
void BSPChangeTexture(room_type* Room, unsigned int ServerID, unsigned short NewTexture, unsigned int Flags)
{
   bool isAboveWall  = ((Flags & CTF_ABOVEWALL) == CTF_ABOVEWALL);
   bool isNormalWall = ((Flags & CTF_NORMALWALL) == CTF_NORMALWALL);
   bool isBelowWall  = ((Flags & CTF_BELOWWALL) == CTF_BELOWWALL);
   bool isFloor      = ((Flags & CTF_FLOOR) == CTF_FLOOR);
   bool isCeiling    = ((Flags & CTF_CEILING) == CTF_CEILING);
   bool isReset      = ((Flags & CTF_RESET) == CTF_RESET);

   // change on sides
   if (isAboveWall || isNormalWall || isBelowWall)
   {
      for (int i = 0; i < Room->SidesCount; i++)
      {
         Side* side = &Room->Sides[i];

         // server ID does not match
         if (side->ServerID != ServerID)
            continue;

         if (isAboveWall)
            side->TextureUpper = (isReset ? side->TextureUpperOrig : NewTexture);

         if (isNormalWall)
            side->TextureMiddle = (isReset ? side->TextureMiddleOrig : NewTexture);

         if (isBelowWall)
            side->TextureLower = (isReset ? side->TextureLowerOrig : NewTexture);
      }
   }

   // change on sectors
   if (isFloor || isCeiling)
   {
      for (int i = 0; i < Room->SectorsCount; i++)
      {
         Sector* sector = &Room->Sectors[i];

         // server ID does not match
         if (sector->ServerID != ServerID)
            continue;

         if (isFloor)
            sector->FloorTexture = (isReset ? sector->FloorTextureOrig : NewTexture);

         if (isCeiling)
            sector->CeilingTexture = (isReset ? sector->CeilingTextureOrig : NewTexture);
      }
   }
}

/*********************************************************************************************/
/* BSPMoveSector:  Adjust floor or ceiling height of a non-sloped sector. 
/*                 Always instant for now. Otherwise only for speed=0. Height must be in 1:1024.
/*********************************************************************************************/
void BSPMoveSector(room_type* Room, unsigned int ServerID, bool Floor, float Height, float Speed)
{
   for (int i = 0; i < Room->SectorsCount; i++)
   {
      Sector* sector = &Room->Sectors[i];

      // server ID does not match
      if (sector->ServerID != ServerID)
         continue;

      // move floor
      if (Floor)
      {
         sector->FloorHeight = Height;
         BSPUpdateLeafHeights(Room, sector, true);
      }

      // move ceiling
      else
      {
         sector->CeilingHeight = Height;
         BSPUpdateLeafHeights(Room, sector, false);
      }
   }
}

/*********************************************************************************************/
/* BSPIsInThingsBox:  Checks if given point lies inside the 'red' boundingbox
/*                    described by the 'Thing' vertices in RoomEdit.
/*********************************************************************************************/
int BSPIsInThingsBox(room_type* Room, V2* P)
{
   if (!Room || !P)
      return IBF_INVALID;

   int flags = IBF_INSIDE;

   // out west
   if (P->X < Room->ThingsBox.Min.X)
      flags |= IBF_OUT_W;

   // out east
   else if (P->X > Room->ThingsBox.Max.X)
      flags |= IBF_OUT_E;

   // out north
   if (P->Y < Room->ThingsBox.Min.Y)
      flags |= IBF_OUT_N;

   // out south
   else if (P->Y > Room->ThingsBox.Max.Y)
      flags |= IBF_OUT_S;

   return flags;
}

/*********************************************************************************************/
/* BSPRooFileLoadServer:  Fill "room" with server-relevant data from given roo file.         */
/*********************************************************************************************/
bool BSPLoadRoom(char *fname, room_type *room)
{
   int i, j, temp;
   unsigned char byte;
   unsigned short unsigshort;
   int offset_client, offset_tree, offset_walls, offset_sides, offset_sectors, offset_things;
   char tmpbuf[128];

   FILE *infile = fopen(fname, "rb");
   if (infile == NULL)
      return False;

   /****************************************************************************/
   /*                                HEADER                                    */
   /****************************************************************************/
   
   // check signature
   if (fread(&temp, 1, 4, infile) != 4 || temp != ROO_SIGNATURE)
   { fclose(infile); return False; }

   // check version
   if (fread(&temp, 1, 4, infile) != 4 || temp < ROO_VERSION)
   { fclose(infile); return False; }

   // read room security
   if (fread(&room->security, 1, 4, infile) != 4)
   { fclose(infile); return False; }

   // read pointer to client info
   if (fread(&offset_client, 1, 4, infile) != 4)
   { fclose(infile); return False; }

   // skip pointer to server info
   if (fread(&temp, 1, 4, infile) != 4)
   { fclose(infile); return False; }

   /****************************************************************************/
   /*                               CLIENT DATA                                */
   /****************************************************************************/
   fseek(infile, offset_client, SEEK_SET);
   
   // skip width
   if (fread(&temp, 1, 4, infile) != 4)
   { fclose(infile); return False; }

   // skip height
   if (fread(&temp, 1, 4, infile) != 4)
   { fclose(infile); return False; }

   // read pointer to bsp tree
   if (fread(&offset_tree, 1, 4, infile) != 4)
   { fclose(infile); return False; }

   // read pointer to walls
   if (fread(&offset_walls, 1, 4, infile) != 4)
   { fclose(infile); return False; }

   // skip offset to editor walls
   if (fread(&temp, 1, 4, infile) != 4)
   { fclose(infile); return False; }

   // read pointer to sides
   if (fread(&offset_sides, 1, 4, infile) != 4)
   { fclose(infile); return False; }

   // read pointer to sectors
   if (fread(&offset_sectors, 1, 4, infile) != 4)
   { fclose(infile); return False; }

   // read pointer to things
   if (fread(&offset_things, 1, 4, infile) != 4)
   { fclose(infile); return False; }

   /************************ BSP-TREE ****************************************/

   fseek(infile, offset_tree, SEEK_SET);

   // read count of nodes
   if (fread(&room->TreeNodesCount, 1, 2, infile) != 2)
   { fclose(infile); return False; }

   // allocate tree mem
   room->TreeNodes = (BspNode*)AllocateMemory(
      MALLOC_ID_ROOM, room->TreeNodesCount * sizeof(BspNode));

   for (i = 0; i < room->TreeNodesCount; i++)
   {
      BspNode* node = &room->TreeNodes[i];

      // type
      if (fread(&byte, 1, 1, infile) != 1)
      { fclose(infile); return False; }
      node->Type = (BspNodeType)byte;

      // boundingbox
      if (fread(&node->BoundingBox.Min.X, 1, 4, infile) != 4)
      { fclose(infile); return False; }
      if (fread(&node->BoundingBox.Min.Y, 1, 4, infile) != 4)
      { fclose(infile); return False; }
      if (fread(&node->BoundingBox.Max.X, 1, 4, infile) != 4)
      { fclose(infile); return False; }
      if (fread(&node->BoundingBox.Max.Y, 1, 4, infile) != 4)
      { fclose(infile); return False; }

      if (node->Type == BspInternalType)
      {
         // line equation coefficients of splitter line
         if (fread(&node->u.internal.A, 1, 4, infile) != 4)
         { fclose(infile); return False; }
         if (fread(&node->u.internal.B, 1, 4, infile) != 4)
         { fclose(infile); return False; }
         if (fread(&node->u.internal.C, 1, 4, infile) != 4)
         { fclose(infile); return False; }

         // nums of children
         if (fread(&node->u.internal.RightChildNum, 1, 2, infile) != 2)
         { fclose(infile); return False; }
         if (fread(&node->u.internal.LeftChildNum, 1, 2, infile) != 2)
         { fclose(infile); return False; }

         // first wall in splitter
         if (fread(&node->u.internal.FirstWallNum, 1, 2, infile) != 2)
         { fclose(infile); return False; }
      }
      else if (node->Type == BspLeafType)
      {
         // sector num
         if (fread(&node->u.leaf.SectorNum, 1, 2, infile) != 2)
         { fclose(infile); return False; }

         // points count
         if (fread(&node->u.leaf.PointsCount, 1, 2, infile) != 2)
         { fclose(infile); return False; }

         // allocate memory for points of polygon
         node->u.leaf.PointsFloor = (V3*)AllocateMemory(
            MALLOC_ID_ROOM, node->u.leaf.PointsCount * sizeof(V3));
         node->u.leaf.PointsCeiling = (V3*)AllocateMemory(
            MALLOC_ID_ROOM, node->u.leaf.PointsCount * sizeof(V3));

         // read points
         for (j = 0; j < node->u.leaf.PointsCount; j++)
         {
            if (fread(&node->u.leaf.PointsFloor[j].X, 1, 4, infile) != 4)
            { fclose(infile); return False; }
            if (fread(&node->u.leaf.PointsFloor[j].Y, 1, 4, infile) != 4)
            { fclose(infile); return False; }
			   
            // x,y are same on floor/ceiling
            node->u.leaf.PointsCeiling[j].X = node->u.leaf.PointsFloor[j].X;
            node->u.leaf.PointsCeiling[j].Y = node->u.leaf.PointsFloor[j].Y;
         }
      }
   }

   /*************************** WALLS ****************************************/
   
   fseek(infile, offset_walls, SEEK_SET);

   // count of walls
   if (fread(&room->WallsCount, 1, 2, infile) != 2)
   { fclose(infile); return False; }

   // allocate walls mem
   room->Walls = (Wall*)AllocateMemory(
      MALLOC_ID_ROOM, room->WallsCount * sizeof(Wall));

   for (i = 0; i < room->WallsCount; i++)
   {
      Wall* wall = &room->Walls[i];

      // save 1-based num for debugging
      wall->Num = i + 1;

      // nextwallinplane num
      if (fread(&wall->NextWallInPlaneNum, 1, 2, infile) != 2)
      { fclose(infile); return False; }

      // side nums
      if (fread(&wall->RightSideNum, 1, 2, infile) != 2)
      { fclose(infile); return False; }
      if (fread(&wall->LeftSideNum, 1, 2, infile) != 2)
      { fclose(infile); return False; }

      // endpoints
      if (fread(&wall->P1.X, 1, 4, infile) != 4)
      { fclose(infile); return False; }
      if (fread(&wall->P1.Y, 1, 4, infile) != 4)
      { fclose(infile); return False; }
      if (fread(&wall->P2.X, 1, 4, infile) != 4)
      { fclose(infile); return False; }
      if (fread(&wall->P2.Y, 1, 4, infile) != 4)
      { fclose(infile); return False; }

      // skip length
      if (fread(&temp, 1, 4, infile) != 4)
      { fclose(infile); return False; }

      // skip texture offsets
      if (fread(&temp, 1, 2, infile) != 2)
      { fclose(infile); return False; }
      if (fread(&temp, 1, 2, infile) != 2)
      { fclose(infile); return False; }
      if (fread(&temp, 1, 2, infile) != 2)
      { fclose(infile); return False; }
      if (fread(&temp, 1, 2, infile) != 2)
      { fclose(infile); return False; }

      // sector nums
      if (fread(&wall->RightSectorNum, 1, 2, infile) != 2)
      { fclose(infile); return False; }
      if (fread(&wall->LeftSectorNum, 1, 2, infile) != 2)
      { fclose(infile); return False; }
   }

   /***************************** SIDES ****************************************/

   fseek(infile, offset_sides, SEEK_SET);

   // count of sides
   if (fread(&room->SidesCount, 1, 2, infile) != 2)
   { fclose(infile); return False; }

   // allocate sides mem
   room->Sides = (Side*)AllocateMemory(
      MALLOC_ID_ROOM, room->SidesCount * sizeof(Side));

   for (i = 0; i < room->SidesCount; i++)
   {
      Side* side = &room->Sides[i];

      // serverid
      if (fread(&side->ServerID, 1, 2, infile) != 2)
      { fclose(infile); return False; }

      // middle,upper,lower texture
      if (fread(&side->TextureMiddle, 1, 2, infile) != 2)
      { fclose(infile); return False; }
      if (fread(&side->TextureUpper, 1, 2, infile) != 2)
      { fclose(infile); return False; }
      if (fread(&side->TextureLower, 1, 2, infile) != 2)
      { fclose(infile); return False; }

      // keep track of original texture nums (can change at runtime)
	  side->TextureLowerOrig  = side->TextureLower;
	  side->TextureMiddleOrig = side->TextureMiddle;
	  side->TextureUpperOrig  = side->TextureUpper;

      // flags
     if (fread(&side->Flags, 1, 4, infile) != 4)
      { fclose(infile); return False; }

      // skip speed byte
     if (fread(&temp, 1, 1, infile) != 1)
      { fclose(infile); return False; }
   }

   /***************************** SECTORS ****************************************/

   fseek(infile, offset_sectors, SEEK_SET);

   // count of sectors
   if (fread(&room->SectorsCount, 1, 2, infile) != 2)
   { fclose(infile); return False; }

   // allocate sectors mem
   room->Sectors = (Sector*)AllocateMemory(
      MALLOC_ID_ROOM, room->SectorsCount * sizeof(Sector));

   for (i = 0; i < room->SectorsCount; i++)
   {
      Sector* sector = &room->Sectors[i];
	   
      // serverid
      if (fread(&sector->ServerID, 1, 2, infile) != 2)
      { fclose(infile); return False; }

      // floor+ceiling texture
      if (fread(&sector->FloorTexture, 1, 2, infile) != 2)
      { fclose(infile); return False; }
      if (fread(&sector->CeilingTexture, 1, 2, infile) != 2)
      { fclose(infile); return False; }

	  // keep track of original texture nums (can change at runtime)
      sector->FloorTextureOrig   = sector->FloorTexture;
      sector->CeilingTextureOrig = sector->CeilingTexture;

      // skip texture offsets
      if (fread(&temp, 1, 2, infile) != 2)
      { fclose(infile); return False; }
      if (fread(&temp, 1, 2, infile) != 2)
      { fclose(infile); return False; }

      // floor+ceiling heights (from 1:64 to 1:1024 like the rest)
      if (fread(&unsigshort, 1, 2, infile) != 2)
      { fclose(infile); return False; }
      sector->FloorHeight = FINENESSKODTOROO((float)unsigshort);
      if (fread(&unsigshort, 1, 2, infile) != 2)
      { fclose(infile); return False; }
      sector->CeilingHeight = FINENESSKODTOROO((float)unsigshort);

      // skip light byte
      if (fread(&temp, 1, 1, infile) != 1)
      { fclose(infile); return False; }

      // flags
      if (fread(&sector->Flags, 1, 4, infile) != 4)
      { fclose(infile); return False; }

      // skip speed byte
      if (fread(&temp, 1, 1, infile) != 1)
      { fclose(infile); return False; }
	   
      // possibly load floor slopeinfo
      if ((sector->Flags & SF_SLOPED_FLOOR) == SF_SLOPED_FLOOR)
      {
         sector->SlopeInfoFloor = (SlopeInfo*)AllocateMemory(
            MALLOC_ID_ROOM, sizeof(SlopeInfo));

         // read 3d plane equation coefficients (normal vector)
         if (fread(&sector->SlopeInfoFloor->A, 1, 4, infile) != 4)
         { fclose(infile); return False; }
         if (fread(&sector->SlopeInfoFloor->B, 1, 4, infile) != 4)
         { fclose(infile); return False; }
         if (fread(&sector->SlopeInfoFloor->C, 1, 4, infile) != 4)
         { fclose(infile); return False; }
         if (fread(&sector->SlopeInfoFloor->D, 1, 4, infile) != 4)
         { fclose(infile); return False; }

         // skip x0, y0, textureangle
         if (fread(&temp, 1, 4, infile) != 4)
         { fclose(infile); return False; }
         if (fread(&temp, 1, 4, infile) != 4)
         { fclose(infile); return False; }
         if (fread(&temp, 1, 4, infile) != 4)
         { fclose(infile); return False; }

         // skip unused payload (vertex indices for roomedit)
         if (fread(&tmpbuf, 1, 18, infile) != 18)
         { fclose(infile); return False; }
      }
      else
         sector->SlopeInfoFloor = NULL;

      // possibly load ceiling slopeinfo
      if ((sector->Flags & SF_SLOPED_CEILING) == SF_SLOPED_CEILING)
      {
         sector->SlopeInfoCeiling = (SlopeInfo*)AllocateMemory(
            MALLOC_ID_ROOM, sizeof(SlopeInfo));

         // read 3d plane equation coefficients (normal vector)
         if (fread(&sector->SlopeInfoCeiling->A, 1, 4, infile) != 4)
         { fclose(infile); return False; }
         if (fread(&sector->SlopeInfoCeiling->B, 1, 4, infile) != 4)
         { fclose(infile); return False; }
         if (fread(&sector->SlopeInfoCeiling->C, 1, 4, infile) != 4)
         { fclose(infile); return False; }
         if (fread(&sector->SlopeInfoCeiling->D, 1, 4, infile) != 4)
         { fclose(infile); return False; }

         // skip x0, y0, textureangle
         if (fread(&temp, 1, 4, infile) != 4)
         { fclose(infile); return False; }
         if (fread(&temp, 1, 4, infile) != 4)
         { fclose(infile); return False; }
         if (fread(&temp, 1, 4, infile) != 4)
         { fclose(infile); return False;}

         // skip unused payload (vertex indices for roomedit)
         if (fread(&tmpbuf, 1, 18, infile) != 18)
         { fclose(infile); return False; }
      }
      else
         sector->SlopeInfoCeiling = NULL;
   }

   /***************************** THINGS ****************************************/
   
   fseek(infile, offset_things, SEEK_SET);

   // count of things
   if (fread(&unsigshort, 1, 2, infile) != 2)
   { fclose(infile); return False; }

   // must have exactly two things describing bbox (each thing a vertex)
   if (unsigshort != 2)
   { fclose(infile); return False; }

   // note: Things vertices are stored as INT in (1:64) fineness, based on the
   // coordinate-system origin AS SHOWN IN ROOMEDIT (Y-UP).
   // Also these can be ANY variant of the 2 possible sets describing
   // a diagonal in a rectangle, so not guaranteed to be ordered like min/or max first.
   float x0, x1, y0, y1;

   if (fread(&temp, 1, 4, infile) != 4)
   { fclose(infile); return False; }
   x0 = (float)temp;
   if (fread(&temp, 1, 4, infile) != 4)
   { fclose(infile); return False; }
   y0 = (float)temp;
   if (fread(&temp, 1, 4, infile) != 4)
   { fclose(infile); return False; }
   x1 = (float)temp;
   if (fread(&temp, 1, 4, infile) != 4)
   { fclose(infile); return False; }
   y1 = (float)temp;
   
   // from the 4 bbox points shown in roomedit (defined by 2 vertices)
   // 1) Pick the left-bottom one as minimum (and scale to ROO fineness)
   // 2) Pick the right-up one as maximum (and scale to ROO fineness)
   room->ThingsBox.Min.X = FINENESSKODTOROO(fmin(x0, x1));
   room->ThingsBox.Min.Y = FINENESSKODTOROO(fmin(y0, y1));
   room->ThingsBox.Max.X = FINENESSKODTOROO(fmax(x0, x1));
   room->ThingsBox.Max.Y = FINENESSKODTOROO(fmax(y0, y1));

   // when roomedit saves the ROO, it translates the origin (0/0)
   // into one boundingbox point, so that origin in ROO (0/0)
   // later is roughly equal to (row=1 col=1)
   
   // translate box so minimum is at (0/0)
   room->ThingsBox.Max.X = room->ThingsBox.Max.X - room->ThingsBox.Min.X;
   room->ThingsBox.Max.Y = room->ThingsBox.Max.Y - room->ThingsBox.Min.Y;
   room->ThingsBox.Min.X = 0.0f;
   room->ThingsBox.Min.Y = 0.0f;

   // calculate the old cols/rows values rather than loading them
   room->cols = (int)(room->ThingsBox.Max.X / 1024.0f);
   room->rows = (int)(room->ThingsBox.Max.Y / 1024.0f);
   room->colshighres = (int)(room->ThingsBox.Max.X / 256.0f);
   room->rowshighres = (int)(room->ThingsBox.Max.Y / 256.0f);

   /************************** DONE READNG **********************************/

   fclose(infile);

   /*************************************************************************/
   /*                      RESOLVE NUMS TO POINTERS                         */
   /*************************************************************************/

   // walls
   for (int i = 0; i < room->WallsCount; i++)
   {
      Wall* wall = &room->Walls[i];

      // right sector
      if (wall->RightSectorNum > 0 &&
          room->SectorsCount > wall->RightSectorNum - 1)
            wall->RightSector = &room->Sectors[wall->RightSectorNum - 1];
      else
         wall->RightSector = NULL;

      // left sector
      if (wall->LeftSectorNum > 0 &&
         room->SectorsCount > wall->LeftSectorNum - 1)
            wall->LeftSector = &room->Sectors[wall->LeftSectorNum - 1];
      else
         wall->LeftSector = NULL;

      // right side
      if (wall->RightSideNum > 0 &&
         room->SidesCount > wall->RightSideNum - 1)
            wall->RightSide = &room->Sides[wall->RightSideNum - 1];
      else
         wall->RightSide = NULL;

      // left side
      if (wall->LeftSideNum > 0 &&
         room->SidesCount > wall->LeftSideNum - 1)
            wall->LeftSide = &room->Sides[wall->LeftSideNum - 1];
      else
         wall->LeftSide = NULL;

      // next wall in splitter
      if (wall->NextWallInPlaneNum > 0 &&
         room->WallsCount > wall->NextWallInPlaneNum - 1)
            wall->NextWallInPlane = &room->Walls[wall->NextWallInPlaneNum - 1];
      else
         wall->NextWallInPlane = NULL;
   }

   // bsp nodes
   for (int i = 0; i < room->TreeNodesCount; i++)
   {
      BspNode* node = &room->TreeNodes[i];

      // internal nodes
      if (node->Type == BspInternalType)
      {
         // first wall
         if (node->u.internal.FirstWallNum > 0 &&
             room->WallsCount > node->u.internal.FirstWallNum - 1)
               node->u.internal.FirstWall = &room->Walls[node->u.internal.FirstWallNum - 1];
         else
            node->u.internal.FirstWall = NULL;

         // right child
         if (node->u.internal.RightChildNum > 0 &&
             room->TreeNodesCount > node->u.internal.RightChildNum - 1)
               node->u.internal.RightChild = &room->TreeNodes[node->u.internal.RightChildNum - 1];
         else
            node->u.internal.RightChild = NULL;

         // left child
         if (node->u.internal.LeftChildNum > 0 &&
             room->TreeNodesCount > node->u.internal.LeftChildNum - 1)
               node->u.internal.LeftChild = &room->TreeNodes[node->u.internal.LeftChildNum - 1];
         else
            node->u.internal.LeftChild = NULL;
      }

      // leafs
      else if (node->Type == BspLeafType)
      {
         // sector this leaf belongs to
         if (node->u.leaf.SectorNum > 0 &&
             room->SectorsCount > node->u.leaf.SectorNum - 1)
               node->u.leaf.Sector = &room->Sectors[node->u.leaf.SectorNum - 1];
         else
            node->u.leaf.Sector = NULL;
      }
   }

   /*************************************************************************/
   /*                RESOLVE HEIGHTS OF LEAF POLY POINTS                    */
   /*************************************************************************/

   for (int i = 0; i < room->TreeNodesCount; i++)
   {
      BspNode* node = &room->TreeNodes[i];

      if (node->Type != BspLeafType)
         continue;

      for (int j = 0; j < node->u.leaf.PointsCount; j++)
      {
         if (!node->u.leaf.Sector)
            continue;

         V2 p = { node->u.leaf.PointsFloor[j].X, node->u.leaf.PointsFloor[j].Y };

         node->u.leaf.PointsFloor[j].Z = 
            SECTORHEIGHTFLOOR(node->u.leaf.Sector, &p);

         node->u.leaf.PointsCeiling[j].Z =
            SECTORHEIGHTCEILING(node->u.leaf.Sector, &p);
      }
   }

   /****************************************************************************/
   /****************************************************************************/

   return True;
}

/*********************************************************************************************/
/* BSPRoomFreeServer:  Free the parts of a room structure used by the server.                */
/*********************************************************************************************/
void BSPFreeRoom(room_type *room)
{
   int i;

   /****************************************************************************/
   /*                               CLIENT PARTS                               */
   /****************************************************************************/
   
   // free bsp nodes 'submem'
   for (i = 0; i < room->TreeNodesCount; i++)
   {
      if (room->TreeNodes[i].Type == BspLeafType)
      {
         FreeMemory(MALLOC_ID_ROOM, room->TreeNodes[i].u.leaf.PointsFloor,
            room->TreeNodes[i].u.leaf.PointsCount * sizeof(V3));
         FreeMemory(MALLOC_ID_ROOM, room->TreeNodes[i].u.leaf.PointsCeiling,
            room->TreeNodes[i].u.leaf.PointsCount * sizeof(V3));
      }
   }

   // free sectors submem
   for (i = 0; i < room->SectorsCount; i++)
   {
      if ((room->Sectors[i].Flags & SF_SLOPED_FLOOR) == SF_SLOPED_FLOOR)
         FreeMemory(MALLOC_ID_ROOM, room->Sectors[i].SlopeInfoFloor, sizeof(SlopeInfo));
      if ((room->Sectors[i].Flags & SF_SLOPED_CEILING) == SF_SLOPED_CEILING)
         FreeMemory(MALLOC_ID_ROOM, room->Sectors[i].SlopeInfoCeiling, sizeof(SlopeInfo));
   }

   FreeMemory(MALLOC_ID_ROOM, room->TreeNodes, room->TreeNodesCount * sizeof(BspNode));
   FreeMemory(MALLOC_ID_ROOM, room->Walls, room->WallsCount * sizeof(Wall));
   FreeMemory(MALLOC_ID_ROOM, room->Sides, room->SidesCount * sizeof(Side));
   FreeMemory(MALLOC_ID_ROOM, room->Sectors, room->SectorsCount * sizeof(Sector));

   room->TreeNodesCount = 0;
   room->WallsCount = 0;
   room->SidesCount = 0;
   room->SectorsCount = 0;

   /****************************************************************************/
   /*                               SERVER PARTS                               */
   /****************************************************************************/
  
   room->rows = 0;
   room->cols = 0;
   room->rowshighres = 0;
   room->colshighres = 0;
   room->resource_id = 0;
   room->roomdata_id = 0;
}
#pragma endregion
