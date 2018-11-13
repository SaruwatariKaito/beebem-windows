/****************************************************************
BeebEm - BBC Micro and Master 128 Emulator
Copyright (C) 2006  Jon Welch

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public 
License along with this program; if not, write to the Free 
Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
Boston, MA  02110-1301, USA.
****************************************************************/

/* Teletext Support for Beebem */

/*

Offset  Description                 Access  
+00     Status register             R/W
+01     Row register
+02     Data register
+03     Clear status register

Status register:
  Read
   Bits     Function
   0-3      Link settings
   4        FSYN (Latches high on Field sync)
   5        DEW (Data entry window)
   6        DOR (Latches INT on end of DEW)
   7        INT (latches high on end of DEW)
  
  Write
   Bits     Function
   0-1      Channel select
   2        Teletext Enable
   3        Enable Interrupts
   4        Enable AFC (and mystery links A)
   5        Mystery links B

*/

#include <stdio.h>
#include <stdlib.h>
#include <process.h>
#include <windows.h>
#include "teletext.h"
#include "debug.h"
#include "6502core.h"
#include "main.h"
#include "beebmem.h"
#include "log.h"

bool TeleTextAdapterEnabled = false;
bool TeletextFiles;
bool TeletextLocalhost;
bool TeletextCustom;

int TeleTextStatus = 0x0f;
bool TeleTextInts = false;
bool TeleTextEnable = false;
int txtChnl = 0;
int rowPtrOffset = 0x00;
int rowPtr = 0x00;
int colPtr = 0x00;

FILE *txtFile = NULL;
long txtFrames = 0;
long txtCurFrame = 0;

unsigned char row[16][64] = {0};

char TeletextIP[4][20] = { "127.0.0.1", "127.0.0.1", "127.0.0.1", "127.0.0.1" };
u_short TeletextPort[4] = { 19761, 19762, 19763, 19764 };
char TeletextCustomIP[4][20];
u_short TeletextCustomPort[4];

extern WSADATA WsaDat;
static SOCKET TeletextSocket[4] = {INVALID_SOCKET, INVALID_SOCKET, INVALID_SOCKET, INVALID_SOCKET};
static unsigned int TeletextConnectThreadID[4];
const int TeletextConnectThreadCh[4] = {0,1,2,3}; // dumb way to get fixed channel numbers into TeletextConnect threads
static unsigned int __stdcall TeletextConnect(void *chparam);

void TeleTextLog(char *text, ...)
{
FILE *f;
va_list argptr;

    return;

	va_start(argptr, text);

    f = fopen("teletext.log", "at");
    if (f)
    {
        vfprintf(f, text, argptr);
        fclose(f);
    }

	va_end(argptr);
}

static unsigned int __stdcall TeletextConnect(void *chparam)
{
    /* initiate connection on socket */
    char info[200];
    int ch = *((int *)chparam);
    
    struct sockaddr_in teletext_serv_addr;
    u_long iMode;
    TeletextSocket[ch] = socket(AF_INET, SOCK_STREAM, 0);
    if (TeletextSocket[ch] == INVALID_SOCKET)
    {
        if (DebugEnabled)
        {
            sprintf(info, "Teletext: Unable to create socket %d", ch);
            DebugDisplayTrace(DebugType::Teletext, true, info);
        }
        _endthreadex(0);
        return 1;
    }
    if (DebugEnabled)
    {
        sprintf(info, "Teletext: socket %d created", ch);
        DebugDisplayTrace(DebugType::Teletext, true, info);
    }
    
    teletext_serv_addr.sin_family = AF_INET; // address family Internet
    teletext_serv_addr.sin_port = htons (TeletextPort[ch]); //Port to connect on
    teletext_serv_addr.sin_addr.s_addr = inet_addr (TeletextIP[ch]); //Target IP
    
    if (connect(TeletextSocket[ch], (SOCKADDR *)&teletext_serv_addr, sizeof(teletext_serv_addr)) == SOCKET_ERROR)
    {
        if (DebugEnabled) {
            sprintf(info, "Teletext: Socket %d unable to connect to server %s:%d %d",ch,TeletextIP[ch], TeletextPort[ch], WSAGetLastError());
            DebugDisplayTrace(DebugType::Teletext, true, info);
        }
        closesocket(TeletextSocket[ch]);
        TeletextSocket[ch] = INVALID_SOCKET;
        _endthreadex(0);
        return 1;
    }

    if (DebugEnabled)
    {
        sprintf(info, "Teletext: socket %d connected to server",ch);
        DebugDisplayTrace(DebugType::Teletext, true, info);
    }
    
    iMode = 1;
    ioctlsocket(TeletextSocket[ch], FIONBIO, &iMode); // non blocking
    
    _endthreadex(0);
    return 0;
}

void TeleTextInit(void)
{
    int i;
    TeleTextStatus = 0x0f; /* low nibble comes from LK4-7 and mystery links which are left floating */
    TeleTextInts = false;
    TeleTextEnable = false;
    txtChnl = 0;
    
    if (!TeleTextAdapterEnabled)
        return;
    
    TeleTextClose();
    for (i=0; i<4; i++){
        if (TeletextCustom)
        {
            strcpy(TeletextIP[i], TeletextCustomIP[i]);
            TeletextPort[i] = TeletextCustomPort[i];
        }
    }
    if (TeletextLocalhost || TeletextCustom)
    {
        if (WSAStartup(MAKEWORD(1, 1), &WsaDat) != 0) {
            WriteLog("Teletext: WSA initialisation failed");
            if (DebugEnabled) 
                DebugDisplayTrace(DebugType::Teletext, true, "Teletext: WSA initialisation failed");
            
            return;
        }
        
        for (i=0; i<4; i++){
            /* start thread to handle connecting each socket to avoid holding up main thread */
            _beginthreadex(nullptr, 0, TeletextConnect, (void *)(&TeletextConnectThreadCh[i]), 0, &TeletextConnectThreadID[i]);
        }
    }
    else
    {
        // TODO: reimplement capture files
    }
    /*
    rowPtr = 0x00;
    colPtr = 0x00;
    
    if (txtFile) fclose(txtFile);

    if (!TeleTextAdapterEnabled)
        return;

    sprintf(buff, "%s/discims/txt%d.dat", mainWin->GetUserDataPath(), txtChnl);
    
    txtFile = fopen(buff, "rb");

    if (txtFile)
    {
        fseek(txtFile, 0L, SEEK_END);
        txtFrames = ftell(txtFile) / 860L;
        fseek(txtFile, 0L, SEEK_SET);
    }

    txtCurFrame = 0;

    TeleTextLog("TeleTextInit Frames = %ld\n", txtFrames);
    */
}

void TeleTextClose()
{
    /* close all connected teletext sockets */
    int ch;
    for (ch=0; ch<4; ch++)
    {
        if (TeletextSocket[ch] != INVALID_SOCKET) {
            if (DebugEnabled)
            {
                char info[200];
                sprintf(info, "Teletext: closing socket %d", ch);
                DebugDisplayTrace(DebugType::Teletext, true, info);
            }
            closesocket(TeletextSocket[ch]);
            TeletextSocket[ch] = INVALID_SOCKET;
        }
    }
    WSACleanup();
}

void TeleTextWrite(int Address, int Value) 
{
    if (!TeleTextAdapterEnabled)
        return;

    TeleTextLog("TeleTextWrite Address = 0x%02x, Value = 0x%02x, PC = 0x%04x\n", Address, Value, ProgramCounter);
	
    switch (Address)
    {
		case 0x00:
            // Status register
            // if (Value * 0x20) mystery links
            // if (Value * 0x10) enable AFC and mystery links
            
            TeleTextInts = (Value & 0x08) == 0x08;
            if (TeleTextInts && (TeleTextStatus & 0x80))
                intStatus|=(1<<teletext);   // interupt if INT and interrupts enabled
            else
                intStatus&=~(1<<teletext);  // Clear interrupt
            
            TeleTextEnable = (Value & 0x04) == 0x04;
            
            if ( (Value & 0x03) != txtChnl)
            {
                txtChnl = Value & 0x03;
            }
            break;
            
		case 0x01:
            rowPtr = Value;
            colPtr = 0x00;
            break;
		case 0x02:
            row[rowPtr][colPtr++] = Value & 0xFF;
            break;
		case 0x03:
            TeleTextStatus &= ~0xD0;       // Clear INT, DOR, and FSYN latches
            intStatus&=~(1<<teletext);     // Clear interrupt
			break;
    }
}

int TeleTextRead(int Address)
{
    if (!TeleTextAdapterEnabled)
        return 0xff;

    int data = 0x00;

    switch (Address)
    {
    case 0x00 :         // Status Register
        data = TeleTextStatus;
        break;
    case 0x01:			// Row Register
        break;
    case 0x02:          // Data Register
        if (colPtr == 0x00)
            TeleTextLog("TeleTextRead Reading Row %d, PC = 0x%04x\n", rowPtr, ProgramCounter);
//        TeleTextLog("TeleTextRead Returning Row %d, Col %d, Data %d, PC = 0x%04x\n", rowPtr, colPtr, row[rowPtr][colPtr], ProgramCounter);
        
        data = row[rowPtr][colPtr++];
        break;

    case 0x03:
        TeleTextStatus &= ~0xD0;       // Clear INT, DOR, and FSYN latches
        intStatus&=~(1<<teletext);
        break;
    }

    TeleTextLog("TeleTextRead Address = 0x%02x, Value = 0x%02x, PC = 0x%04x\n", Address, data, ProgramCounter);
    
    return data;
}

void TeleTextPoll(void)
{
    if (!TeleTextAdapterEnabled)
        return;

    int i;
    //char buff[16 * 43];
    char socketBuff[4][672] = {0};
    int ret;
    
    if (TeletextLocalhost || TeletextCustom)
    {
        for (i=0;i<4;i++)
        {
            if (TeletextSocket[i] != INVALID_SOCKET)
            {
                ret = recv(TeletextSocket[i], socketBuff[i], 672, 0);
                // todo: something sensible with ret
                TeleTextStatus &= 0x0F;
                TeleTextStatus |= 0xD0;       // data ready so latch INT, DOR, and FSYN
            }
        }
        
        if (TeleTextEnable == true)
        {
            for (i = 0; i < 16; ++i)
            {
                if (socketBuff[txtChnl][i*42] != 0)
                {
                    row[i][0] = 0x27;
                    memcpy(&(row[i][1]), socketBuff[txtChnl] + i * 42, 42);
                }
            }
        }
    }
    else
    {
        // TODO: reimplement capture files
    }
    
    if (TeleTextInts == true)
        intStatus|=1<<teletext;
    
    /*
    if (txtFile)
    {

        if (TeleTextInts == true)
        {


            intStatus|=1<<teletext;

//            TeleTextStatus = 0xef;
            rowPtr = 0x00;
            colPtr = 0x00;

            TeleTextLog("TeleTextPoll Reading Frame %ld, PC = 0x%04x\n", txtCurFrame, ProgramCounter);

            fseek(txtFile, txtCurFrame * 860L + 3L * 43L, SEEK_SET);
            fread(buff, 16 * 43, 1, txtFile);
            for (i = 0; i < 16; ++i)
            {
                if (buff[i*43] != 0)
                {
                    row[i][0] = 0x67;
                    memcpy(&(row[i][1]), buff + i * 43, 42);
                }
                else
                {
                    row[i][0] = 0x00;
                }
            }
        
            txtCurFrame++;
            if (txtCurFrame >= txtFrames) txtCurFrame = 0;
        }
    }
    */

}
