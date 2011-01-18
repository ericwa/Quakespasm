/*
	cd_null.c
	$Id: cd_null.c,v 1.6 2007/06/01 15:55:08 sezero Exp $

	This program is free software; you can redistribute it and/or
	modify it under the terms of the GNU General Public License
	as published by the Free Software Foundation; either version 2
	of the License, or (at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

	See the GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program; if not, write to:

		Free Software Foundation, Inc.
		59 Temple Place - Suite 330
		Boston, MA  02111-1307, USA

*/


#include "quakedef.h"

void CDAudioBackend_Eject(void)
{
}

void CDAudioBackend_Play(byte track, qboolean looping)
{
}

qboolean CDAudioBackend_IsPlaying()
{
	return false;
}

void CDAudioBackend_Stop(void)
{
}

void CDAudioBackend_Next(void)
{
}

void CDAudioBackend_Prev(void)
{
}

void CDAudioBackend_Pause(void)
{
}

void CDAudioBackend_Resume(void)
{
}

void CDAudioBackend_Info(void)
{
}

void CDAudioBackend_Update(void)
{
}

int CDAudioBackend_Init(void)
{
	Con_Printf("CDAudio disabled at compile time\n");
	return -1;
}

void CDAudioBackend_Shutdown(void)
{
}
