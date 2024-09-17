/* DirectIQ151 is 8-bit computer emulator based on the FabGL library
 * -> see http://www.fabglib.org/ or FabGL on GitHub.
 *  
 * For proper operation, an ESP32 module with a VGA monitor 
 * and a PS2 keyboard connected according to the 
 * diagram on the website www.fabglib.org is required.
 * 
 * DirectIQ151 is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or any later version.
 * DirectIQ151 is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY.
 * Stan Pechal, 2024
 * Version 1.0
*/
#include "fabgl.h"
#include "emudevs/i8080.h"          // For processor

fabgl::VGADirectController DisplayController;
fabgl::PS2Controller PS2Controller;

// Constants for video output
static constexpr int borderSize           = 22;
static constexpr int borderXSize          = 72;
static constexpr int scanlinesPerCallback = 2;  // screen height should be divisible by this value

static TaskHandle_t  mainTaskHandle;
void * auxPoint;
bool ROMsel = true;       // Flag for initial bootload
bool BreakFlag = false;   // Flag for Break key pressed

// **************************************************************************************************

// Hardware emulated on the IQ151 computer
// Processor I8080 will be used from the library FabGL
fabgl::i8080 m_i8080;
// Variables for emulating keyboard connection
int portPA = 0x00;    // Data send to PA port
int portPB = 0x00;    // Data send to PB port
int portPC = 0xFF;    // Data read from PC port (upper keyboard nibble)
int portPCout = 0x00;  // Data send to PC port
int portPCW = 0x00;    // Data send control register
int keyboardInL[8];    // Value read from keyboard PA port
int keyboardInR[8];    // Value read from keyboard PB port

// Variables for video display
uint8_t fgcolor; // character color
uint8_t bgcolor; // background color
uint8_t darkbgcolor; // backplane
int width, height;  // display dimensions

// RAM memory will be just Byte array
uint8_t IQ151ram[65536];   // selected addresses are overwritten by ROM in read mode
// ROM memory is contained in the array "monitorIQ[]"
#include "monitor.h"
// ROM memory of character generator is in the array "videoROM[]"
#include "video.h"
// BASIC memory is contained in the array "basic6[]"
#include "basic6.h"

// **************************************************************************************************
// Functions for communication on the bus
static int readByte(void * context, int address)              { if(ROMsel) return(monitorIQ[(address & 0x7FF) | 0x800]);
                                                                else if(address > 0xEFFF) return(monitorIQ[(address & 0xFFF)]); 
                                                                else if ((address >= 0xC800) && (address < 0xE800)) return(basic6[address - 0xC800]); 
                                                                else return(IQ151ram[address & 0xFFFF]); };
static void writeByte(void * context, int address, int value) { IQ151ram[address & 0xFFFF] = (unsigned char)value; };
static int readWord(void * context, int addr)                 { return readByte(context, addr) | (readByte(context, addr + 1) << 8); };
static void writeWord(void * context, int addr, int value)    { writeByte(context, addr, value & 0xFF); writeByte(context, addr + 1, value >> 8); } ;
// Input/Output
static int readIO(void * context, int address)
{
  int outKey = 0xFF;
  switch (address) {
    case 0x84:      // PA port
      if(!(portPB & 0x01)) outKey &= (keyboardInL[0]);
      if(!(portPB & 0x02)) outKey &= (keyboardInL[1]);
      if(!(portPB & 0x04)) outKey &= (keyboardInL[2]);
      if(!(portPB & 0x08)) outKey &= (keyboardInL[3]);
      if(!(portPB & 0x10)) outKey &= (keyboardInL[4]);
      if(!(portPB & 0x20)) outKey &= (keyboardInL[5]);
      if(!(portPB & 0x40)) outKey &= (keyboardInL[6]);
      if(!(portPB & 0x80)) outKey &= (keyboardInL[7]);
      return outKey;
    break;
    case 0x85:      // PB port
      if(!(portPA & 0x01)) outKey &= (keyboardInR[0]);
      if(!(portPA & 0x02)) outKey &= (keyboardInR[1]);
      if(!(portPA & 0x04)) outKey &= (keyboardInR[2]);
      if(!(portPA & 0x08)) outKey &= (keyboardInR[3]);
      if(!(portPA & 0x10)) outKey &= (keyboardInR[4]);
      if(!(portPA & 0x20)) outKey &= (keyboardInR[5]);
      if(!(portPA & 0x40)) outKey &= (keyboardInR[6]);
      if(!(portPA & 0x80)) outKey &= (keyboardInR[7]);
      return outKey;
    break;
    case 0x86:      // PC port
      if(!(portPCout & 0x06)) return portPC; // Read keyboard through mux
      else return 0x20;    // Read "tape recorder" - not functional
    break;
    default: return 0xFF; break;  // Return "not selected I/O" - bus with 0xFF
  }
};

static void writeIO(void * context, int address, int value)
{
    switch (address) {
    case 0x80:      // Select RAM access
      if(value & 0x01) ROMsel=false; else ROMsel=true;
    break;
    case 0x84:      // PortA
      portPA = value;
    break;
    case 0x85:      // PortB
      portPB = value;
    break;
    case 0x86:      // PortC
      portPCout = value;
      if(value & 0x08) digitalWrite(25, HIGH); else digitalWrite(25, LOW);      // Audio output is very simple :-) ... so it's not perfect
    break;
    case 0x87:      // PortPCW
      portPCW = value;
    break;
    default: break;
  }
};

// Reset microcomputer to initial state (Used after Esc key press)
void resetComputer()
{
    portPA = 0x00;    // Reset 8255 ports
    portPB = 0x00;
    portPC = 0xFF;
    for (int i = 0; i < 8; i++) { keyboardInL[i]=0xFF; keyboardInR[i]=0xFF; }  // Clear keyboard matrix
    BreakFlag = false;  // No Break key
    ROMsel = true;      // Set read from ROM
    m_i8080.reset();    // Reset procesor
};

// **************************************************************************************************
// Keyboard interface for selected keys
// Handles Key Up following keys:
void procesKeyUp(VirtualKey key) {
  switch (key) {
      case VirtualKey::VK_LSHIFT:
      case VirtualKey::VK_RSHIFT: portPC |= 0x10; break;  // L and R shift
      case VirtualKey::VK_LCTRL:
      case VirtualKey::VK_RCTRL: portPC |= 0x20; break;  // Ctrl
      case VirtualKey::VK_LALT: portPC |= 0x40; break;  // Left Alt
      case VirtualKey::VK_RALT: portPC |= 0x80; break;  // Right Alt
    
      case VirtualKey::VK_F1: keyboardInL[4] |= 0x80; keyboardInR[7] |= 0x10; break;  // F1
      case VirtualKey::VK_F2: keyboardInL[5] |= 0x80; keyboardInR[7] |= 0x20; break;  // F2
      case VirtualKey::VK_F3: keyboardInL[7] |= 0x80; keyboardInR[7] |= 0x80; break;  // F3
      case VirtualKey::VK_F4: keyboardInL[6] |= 0x80; keyboardInR[7] |= 0x40; break;  // F4
      case VirtualKey::VK_F5: keyboardInL[6] |= 0x10; keyboardInR[4] |= 0x40; break;  // F5
      case VirtualKey::VK_INSERT: keyboardInL[4] |= 0x40; keyboardInR[6] |= 0x10;  break; // INS
      case VirtualKey::VK_DELETE: keyboardInL[4] |= 0x20; keyboardInR[5] |= 0x10;  break; // DEL
      case VirtualKey::VK_LEFT: keyboardInL[5] |= 0x40; keyboardInR[6] |= 0x20;  break; // < left
      case VirtualKey::VK_HOME: keyboardInL[6] |= 0x40; keyboardInR[6] |= 0x40;  break; // Home
      case VirtualKey::VK_RIGHT: keyboardInL[7] |= 0x40; keyboardInR[6] |= 0x80;  break;// > right 
      case VirtualKey::VK_UP: keyboardInL[7] |= 0x20; keyboardInR[5] |= 0x80;  break; // Up
      case VirtualKey::VK_DOWN: keyboardInL[6] |= 0x08; keyboardInR[3] |= 0x40;  break; // Down 
      case VirtualKey::VK_PAGEUP: keyboardInL[6] |= 0x20; keyboardInR[5] |= 0x40;  break; // PgUp
      case VirtualKey::VK_END: keyboardInL[7] |= 0x10; keyboardInR[4] |= 0x80;   break;// End
      case VirtualKey::VK_PAGEDOWN: keyboardInL[5] |= 0x20; keyboardInR[5] |= 0x20;  break; // PgDown 

      case VirtualKey::VK_KP_0:
      case VirtualKey::VK_RIGHTPAREN:
      case VirtualKey::VK_0: keyboardInL[5] |= 0x01; keyboardInR[0] |= 0x20; break;  // 0
      case VirtualKey::VK_KP_1:
      case VirtualKey::VK_EXCLAIM:
      case VirtualKey::VK_1: keyboardInL[0] |= 0x01; keyboardInR[0] |= 0x01; break;  // 1
      case VirtualKey::VK_KP_2:
      case VirtualKey::VK_AT:
      case VirtualKey::VK_2: keyboardInL[0] |= 0x02; keyboardInR[1] |= 0x01; break;  // 2
      case VirtualKey::VK_KP_3:
      case VirtualKey::VK_HASH:
      case VirtualKey::VK_3: keyboardInL[0] |= 0x04; keyboardInR[2] |= 0x01; break;  // 3
      case VirtualKey::VK_KP_4:
      case VirtualKey::VK_DOLLAR:
      case VirtualKey::VK_4: keyboardInL[0] |= 0x08; keyboardInR[3] |= 0x01; break;  // 4
      case VirtualKey::VK_KP_5:
      case VirtualKey::VK_PERCENT:
      case VirtualKey::VK_5: keyboardInL[0] |= 0x10; keyboardInR[4] |= 0x01; break;  // 5
      case VirtualKey::VK_KP_6:
      case VirtualKey::VK_CARET:
      case VirtualKey::VK_6: keyboardInL[0] |= 0x20; keyboardInR[5] |= 0x01; break;  // 6
      case VirtualKey::VK_KP_7:
      case VirtualKey::VK_AMPERSAND:
      case VirtualKey::VK_7: keyboardInL[0] |= 0x40; keyboardInR[6] |= 0x01; break;  // 7
      case VirtualKey::VK_KP_8:
      case VirtualKey::VK_ASTERISK:
      case VirtualKey::VK_8: keyboardInL[0] |= 0x80; keyboardInR[7] |= 0x01; break;  // 8
      case VirtualKey::VK_KP_9:
      case VirtualKey::VK_LEFTPAREN:
      case VirtualKey::VK_9: keyboardInL[4] |= 0x01; keyboardInR[0] |= 0x10; break;  // 9
      case VirtualKey::VK_UNDERSCORE:
      case VirtualKey::VK_MINUS: keyboardInL[7] |= 0x01; keyboardInR[0] |= 0x80; break;  // -_
      case VirtualKey::VK_GRAVEACCENT:
      case VirtualKey::VK_TILDE: keyboardInL[6] |= 0x01; keyboardInR[0] |= 0x40; break;  // Tilde
      case VirtualKey::VK_VERTICALBAR:
      case VirtualKey::VK_BACKSLASH: keyboardInL[5] |= 0x10; keyboardInR[4] |= 0x20; break;  // |\

      case VirtualKey::VK_q:
      case VirtualKey::VK_Q: keyboardInL[1] |= 0x01; keyboardInR[0] |= 0x02; break;  // q-Q
      case VirtualKey::VK_w:
      case VirtualKey::VK_W: keyboardInL[1] |= 0x02; keyboardInR[1] |= 0x02; break;  // w-W
      case VirtualKey::VK_e:
      case VirtualKey::VK_E: keyboardInL[1] |= 0x04; keyboardInR[2] |= 0x02; break;  // e-E
      case VirtualKey::VK_r:
      case VirtualKey::VK_R: keyboardInL[1] |= 0x08; keyboardInR[3] |= 0x02; break;  // r-R
      case VirtualKey::VK_t:
      case VirtualKey::VK_T: keyboardInL[1] |= 0x10; keyboardInR[4] |= 0x02; break;  // t-T
      case VirtualKey::VK_y:
      case VirtualKey::VK_Y: keyboardInL[1] |= 0x20; keyboardInR[5] |= 0x02; break;  // y-Y
      case VirtualKey::VK_u:
      case VirtualKey::VK_U: keyboardInL[1] |= 0x40; keyboardInR[6] |= 0x02; break;  // u-U
      case VirtualKey::VK_i:
      case VirtualKey::VK_I: keyboardInL[1] |= 0x80; keyboardInR[7] |= 0x02; break;  // i-I
      case VirtualKey::VK_o:
      case VirtualKey::VK_O: keyboardInL[4] |= 0x02; keyboardInR[1] |= 0x10; break;  // o-O
      case VirtualKey::VK_p:
      case VirtualKey::VK_P: keyboardInL[5] |= 0x02; keyboardInR[1] |= 0x20; break;  // p-P
      case VirtualKey::VK_LEFTBRACKET:
      case VirtualKey::VK_LEFTBRACE: keyboardInL[6] |= 0x02; keyboardInR[1] |= 0x40; break;  // [{
      case VirtualKey::VK_RIGHTBRACKET:
      case VirtualKey::VK_RIGHTBRACE: keyboardInL[6] |= 0x04; keyboardInR[2] |= 0x40; break;  // ]}

      case VirtualKey::VK_a:
      case VirtualKey::VK_A: keyboardInL[2] |= 0x01; keyboardInR[0] |= 0x04; break;  // a-A
      case VirtualKey::VK_s:
      case VirtualKey::VK_S: keyboardInL[2] |= 0x02; keyboardInR[1] |= 0x04; break;  // s-S
      case VirtualKey::VK_d:
      case VirtualKey::VK_D: keyboardInL[2] |= 0x04; keyboardInR[2] |= 0x04; break;  // d-D
      case VirtualKey::VK_f:
      case VirtualKey::VK_F: keyboardInL[2] |= 0x08; keyboardInR[3] |= 0x04; break;  // f-F
      case VirtualKey::VK_g:
      case VirtualKey::VK_G: keyboardInL[2] |= 0x10; keyboardInR[4] |= 0x04; break;  // g-G
      case VirtualKey::VK_h:
      case VirtualKey::VK_H: keyboardInL[2] |= 0x20; keyboardInR[5] |= 0x04; break;  // h-H
      case VirtualKey::VK_j:
      case VirtualKey::VK_J: keyboardInL[2] |= 0x40; keyboardInR[6] |= 0x04; break;  // j-J
      case VirtualKey::VK_k:
      case VirtualKey::VK_K: keyboardInL[2] |= 0x80; keyboardInR[7] |= 0x04; break;  // k-K
      case VirtualKey::VK_l:
      case VirtualKey::VK_L: keyboardInL[4] |= 0x04; keyboardInR[2] |= 0x10; break;  // l-L
      case VirtualKey::VK_RETURN:
      case VirtualKey::VK_KP_ENTER: keyboardInL[4] |= 0x10; keyboardInR[4] |= 0x10; break;  // R Enter
      case VirtualKey::VK_EQUALS:
      case VirtualKey::VK_KP_PLUS:
      case VirtualKey::VK_PLUS: keyboardInL[5] |= 0x04; keyboardInR[2] |= 0x20; break;  // +=
      case VirtualKey::VK_COLON:
      case VirtualKey::VK_SEMICOLON: keyboardInL[7] |= 0x02; keyboardInR[1] |= 0x80; break;  // ;-:
      case VirtualKey::VK_QUOTE:
      case VirtualKey::VK_QUOTEDBL: keyboardInL[7] |= 0x04; keyboardInR[2] |= 0x80;; break;  // '-"

      case VirtualKey::VK_z:
      case VirtualKey::VK_Z: keyboardInL[3] |= 0x01; keyboardInR[0] |= 0x08; break;  // z-Z
      case VirtualKey::VK_x:
      case VirtualKey::VK_X: keyboardInL[3] |= 0x02; keyboardInR[1] |= 0x08; break;  // x-X
      case VirtualKey::VK_c:
      case VirtualKey::VK_C: keyboardInL[3] |= 0x04; keyboardInR[2] |= 0x08; break;  // c-C
      case VirtualKey::VK_v:
      case VirtualKey::VK_V: keyboardInL[3] |= 0x08; keyboardInR[3] |= 0x08; break;  // v-V
      case VirtualKey::VK_b:
      case VirtualKey::VK_B: keyboardInL[3] |= 0x10; keyboardInR[4] |= 0x08; break;  // b-B
      case VirtualKey::VK_n:
      case VirtualKey::VK_N: keyboardInL[3] |= 0x20; keyboardInR[5] |= 0x08; break;  // n-N
      case VirtualKey::VK_m:
      case VirtualKey::VK_M: keyboardInL[3] |= 0x40; keyboardInR[6] |= 0x08; break;  // m-M
      case VirtualKey::VK_LESS:
      case VirtualKey::VK_COMMA: keyboardInL[3] |= 0x80; keyboardInR[7] |= 0x08; break;  // <-,
      case VirtualKey::VK_GREATER:
      case VirtualKey::VK_PERIOD: keyboardInL[4] |= 0x08; keyboardInR[3] |= 0x10; break;  // >-.
      case VirtualKey::VK_SLASH:
      case VirtualKey::VK_QUESTION: keyboardInL[5] |= 0x08; keyboardInR[3] |= 0x20; break;  // /-?
      case VirtualKey::VK_SPACE: keyboardInL[7] |= 0x08; keyboardInR[3] |= 0x80; break;  // space
      default: break;
      }
};

// Handles Key Down following keys:
void procesKeyDown(VirtualKey key) {
  switch (key) {
      case VirtualKey::VK_ESCAPE: resetComputer(); break;       // ESC as RESET
      case VirtualKey::VK_PAUSE: BreakFlag= true; break;        // Break

      case VirtualKey::VK_LSHIFT:
      case VirtualKey::VK_RSHIFT: portPC &= 0xEF; break;  // L and R shift
      case VirtualKey::VK_LCTRL:
      case VirtualKey::VK_RCTRL: portPC &= 0xDF; break;  // Ctrl
      case VirtualKey::VK_LALT: portPC &= 0xBF; break;  // Left Alt
      case VirtualKey::VK_RALT: portPC &= 0x7F; break;  // Right Alt

      case VirtualKey::VK_F1: keyboardInL[4] &= 0x7F; keyboardInR[7] &= 0xEF; break;  // F1
      case VirtualKey::VK_F2: keyboardInL[5] &= 0x7F; keyboardInR[7] &= 0xDF; break;  // F2
      case VirtualKey::VK_F3: keyboardInL[7] &= 0x7F; keyboardInR[7] &= 0x7F; break;  // F3
      case VirtualKey::VK_F4: keyboardInL[6] &= 0x7F; keyboardInR[7] &= 0xBF; break;  // F4
      case VirtualKey::VK_F5: keyboardInL[6] &= 0xEF; keyboardInR[4] &= 0xBF; break;  // F5
      case VirtualKey::VK_INSERT: keyboardInL[4] &= 0xBF; keyboardInR[6] &= 0xEF;  break; // INS
      case VirtualKey::VK_DELETE: keyboardInL[4] &= 0xDF; keyboardInR[5] &= 0xEF;  break; // DEL
      case VirtualKey::VK_LEFT: keyboardInL[5] &= 0xBF; keyboardInR[6] &= 0xDF;  break; // < left
      case VirtualKey::VK_HOME: keyboardInL[6] &= 0xBF; keyboardInR[6] &= 0xBF;  break; // Home
      case VirtualKey::VK_RIGHT: keyboardInL[7] &= 0xBF; keyboardInR[6] &= 0x7F;  break;// > right 
      case VirtualKey::VK_UP: keyboardInL[7] &= 0xDF; keyboardInR[5] &= 0x7F;  break; // Up
      case VirtualKey::VK_DOWN: keyboardInL[6] &= 0xF7; keyboardInR[3] &= 0xBF;  break; // Down 
      case VirtualKey::VK_PAGEUP: keyboardInL[6] &= 0xDF; keyboardInR[5] &= 0xBF;  break; // PgUp
      case VirtualKey::VK_END: keyboardInL[7] &= 0xEF; keyboardInR[4] &= 0x7F;   break;// End
      case VirtualKey::VK_PAGEDOWN: keyboardInL[5] &= 0xDF; keyboardInR[5] &= 0xDF;  break; // PgDown 

      case VirtualKey::VK_KP_0:
      case VirtualKey::VK_RIGHTPAREN:
      case VirtualKey::VK_0: keyboardInL[5] &= 0xFE; keyboardInR[0] &= 0xDF; break;  // 0
      case VirtualKey::VK_KP_1:
      case VirtualKey::VK_EXCLAIM:
      case VirtualKey::VK_1: keyboardInL[0] &= 0xFE; keyboardInR[0] &= 0xFE; break;  // 1
      case VirtualKey::VK_KP_2:
      case VirtualKey::VK_AT:
      case VirtualKey::VK_2: keyboardInL[0] &= 0xFD; keyboardInR[1] &= 0xFE; break;  // 2
      case VirtualKey::VK_KP_3:
      case VirtualKey::VK_HASH:
      case VirtualKey::VK_3: keyboardInL[0] &= 0xFB; keyboardInR[2] &= 0xFE; break;  // 3
      case VirtualKey::VK_KP_4:
      case VirtualKey::VK_DOLLAR:
      case VirtualKey::VK_4: keyboardInL[0] &= 0xF7; keyboardInR[3] &= 0xFE; break;  // 4
      case VirtualKey::VK_KP_5:
      case VirtualKey::VK_PERCENT:
      case VirtualKey::VK_5: keyboardInL[0] &= 0xEF; keyboardInR[4] &= 0xFE; break;  // 5
      case VirtualKey::VK_KP_6:
      case VirtualKey::VK_CARET:
      case VirtualKey::VK_6: keyboardInL[0] &= 0xDF; keyboardInR[5] &= 0xFE; break;  // 6
      case VirtualKey::VK_KP_7:
      case VirtualKey::VK_AMPERSAND:
      case VirtualKey::VK_7: keyboardInL[0] &= 0xBF; keyboardInR[6] &= 0xFE; break;  // 7
      case VirtualKey::VK_KP_8:
      case VirtualKey::VK_ASTERISK:
      case VirtualKey::VK_8: keyboardInL[0] &= 0x7F; keyboardInR[7] &= 0xFE; break;  // 8
      case VirtualKey::VK_KP_9:
      case VirtualKey::VK_LEFTPAREN:
      case VirtualKey::VK_9: keyboardInL[4] &= 0xFE; keyboardInR[0] &= 0xEF; break;  // 9
      case VirtualKey::VK_UNDERSCORE:
      case VirtualKey::VK_MINUS: keyboardInL[7] &= 0xFE; keyboardInR[0] &= 0x7F; break;  // -_
      case VirtualKey::VK_GRAVEACCENT:
      case VirtualKey::VK_TILDE: keyboardInL[6] &= 0xFE; keyboardInR[0] &= 0xBF; break;  // Tilde
      case VirtualKey::VK_VERTICALBAR:
      case VirtualKey::VK_BACKSLASH: keyboardInL[5] &= 0xEF; keyboardInR[4] &= 0xDF; break;  // |\


      case VirtualKey::VK_q:
      case VirtualKey::VK_Q: keyboardInL[1] &= 0xFE; keyboardInR[0] &= 0xFD; break;  // q-Q
      case VirtualKey::VK_w:
      case VirtualKey::VK_W: keyboardInL[1] &= 0xFD; keyboardInR[1] &= 0xFD; break;  // w-W
      case VirtualKey::VK_e:
      case VirtualKey::VK_E: keyboardInL[1] &= 0xFB; keyboardInR[2] &= 0xFD; break;  // e-E
      case VirtualKey::VK_r:
      case VirtualKey::VK_R: keyboardInL[1] &= 0xF7; keyboardInR[3] &= 0xFD; break;  // r-R
      case VirtualKey::VK_t:
      case VirtualKey::VK_T: keyboardInL[1] &= 0xEF; keyboardInR[4] &= 0xFD; break;  // t-T
      case VirtualKey::VK_y:
      case VirtualKey::VK_Y: keyboardInL[1] &= 0xDF; keyboardInR[5] &= 0xFD; break;  // y-Y
      case VirtualKey::VK_u:
      case VirtualKey::VK_U: keyboardInL[1] &= 0xBF; keyboardInR[6] &= 0xFD; break;  // u-U
      case VirtualKey::VK_i:
      case VirtualKey::VK_I: keyboardInL[1] &= 0x7F; keyboardInR[7] &= 0xFD; break;  // i-I
      case VirtualKey::VK_o:
      case VirtualKey::VK_O: keyboardInL[4] &= 0xFD; keyboardInR[1] &= 0xEF; break;  // o-O
      case VirtualKey::VK_p:
      case VirtualKey::VK_P: keyboardInL[5] &= 0xFD; keyboardInR[1] &= 0xDF; break;  // p-P
      case VirtualKey::VK_LEFTBRACKET:
      case VirtualKey::VK_LEFTBRACE: keyboardInL[6] &= 0xFD; keyboardInR[1] &= 0xBF; break;  // [{
      case VirtualKey::VK_RIGHTBRACKET:
      case VirtualKey::VK_RIGHTBRACE: keyboardInL[6] &= 0xFB; keyboardInR[2] &= 0xBF; break;  // ]}

      case VirtualKey::VK_a:
      case VirtualKey::VK_A: keyboardInL[2] &= 0xFE; keyboardInR[0] &= 0xFB; break;  // a-A
      case VirtualKey::VK_s:
      case VirtualKey::VK_S: keyboardInL[2] &= 0xFD; keyboardInR[1] &= 0xFB; break;  // s-S
      case VirtualKey::VK_d:
      case VirtualKey::VK_D: keyboardInL[2] &= 0xFB; keyboardInR[2] &= 0xFB; break;  // d-D
      case VirtualKey::VK_f:
      case VirtualKey::VK_F: keyboardInL[2] &= 0xF7; keyboardInR[3] &= 0xFB; break;  // f-F
      case VirtualKey::VK_g:
      case VirtualKey::VK_G: keyboardInL[2] &= 0xEF; keyboardInR[4] &= 0xFB; break;  // g-G
      case VirtualKey::VK_h:
      case VirtualKey::VK_H: keyboardInL[2] &= 0xDF; keyboardInR[5] &= 0xFB; break;  // h-H
      case VirtualKey::VK_j:
      case VirtualKey::VK_J: keyboardInL[2] &= 0xBF; keyboardInR[6] &= 0xFB; break;  // j-J
      case VirtualKey::VK_k:
      case VirtualKey::VK_K: keyboardInL[2] &= 0x7F; keyboardInR[7] &= 0xFB; break;  // k-K
      case VirtualKey::VK_l:
      case VirtualKey::VK_L: keyboardInL[4] &= 0xFB; keyboardInR[2] &= 0xEF; break;  // l-L
      case VirtualKey::VK_RETURN:
      case VirtualKey::VK_KP_ENTER: keyboardInL[4] &= 0xEF; keyboardInR[4] &= 0xEF; break;  // R Enter
      case VirtualKey::VK_EQUALS:
      case VirtualKey::VK_KP_PLUS:
      case VirtualKey::VK_PLUS: keyboardInL[5] &= 0xFB; keyboardInR[2] &= 0xDF; break;  // +=
      case VirtualKey::VK_COLON:
      case VirtualKey::VK_SEMICOLON: keyboardInL[7] &= 0xFD; keyboardInR[1] &= 0x7F; break;  // ;-:
      case VirtualKey::VK_QUOTE:
      case VirtualKey::VK_QUOTEDBL: keyboardInL[7] &= 0xFB; keyboardInR[2] &= 0x7F;; break;  // '-"

      case VirtualKey::VK_z:
      case VirtualKey::VK_Z: keyboardInL[3] &= 0xFE; keyboardInR[0] &= 0xF7; break;  // z-Z
      case VirtualKey::VK_x:
      case VirtualKey::VK_X: keyboardInL[3] &= 0xFD; keyboardInR[1] &= 0xF7; break;  // x-X
      case VirtualKey::VK_c:
      case VirtualKey::VK_C: keyboardInL[3] &= 0xFB; keyboardInR[2] &= 0xF7; break;  // c-C
      case VirtualKey::VK_v:
      case VirtualKey::VK_V: keyboardInL[3] &= 0xF7; keyboardInR[3] &= 0xF7; break;  // v-V
      case VirtualKey::VK_b:
      case VirtualKey::VK_B: keyboardInL[3] &= 0xEF; keyboardInR[4] &= 0xF7; break;  // b-B
      case VirtualKey::VK_n:
      case VirtualKey::VK_N: keyboardInL[3] &= 0xDF; keyboardInR[5] &= 0xF7; break;  // n-N
      case VirtualKey::VK_m:
      case VirtualKey::VK_M: keyboardInL[3] &= 0xBF; keyboardInR[6] &= 0xF7; break;  // m-M
      case VirtualKey::VK_LESS:
      case VirtualKey::VK_COMMA: keyboardInL[3] &= 0x7F; keyboardInR[7] &= 0xF7; break;  // <-,
      case VirtualKey::VK_GREATER:
      case VirtualKey::VK_PERIOD: keyboardInL[4] &= 0xF7; keyboardInR[3] &= 0xEF; break;  // >-.
      case VirtualKey::VK_SLASH:
      case VirtualKey::VK_QUESTION: keyboardInL[5] &= 0xF7; keyboardInR[3] &= 0xDF; break;  // /-?
      case VirtualKey::VK_SPACE: keyboardInL[7] &= 0xF7; keyboardInR[3] &= 0x7F; break;  // space
      default: break;
      }
};

// **************************************************************************************************
// VGA main function - prepare lines for displaying
void IRAM_ATTR drawScanline(void * arg, uint8_t * dest, int scanLine)
{
  // draws "scanlinesPerCallback" scanlines every time drawScanline() is called
  for (int i = 0; i < scanlinesPerCallback; ++i) {
    // fill border with background color
    memset(dest, darkbgcolor, width);
    if ((scanLine >= borderSize) && (scanLine < (256 + borderSize)))      // Display is 32 char lines * 8 pixel rows hight
      { // Prepare row with background color
        memset(dest+borderXSize, fgcolor, 256);
        int displayLine = scanLine - borderSize;
        int pixelPointer = 0;   // pointer to pixel on a row
        int memoryPointer = 0xEC00 + ((displayLine / 8) << 5);   // Start of line in memory
        do {
          uint8_t videobyte = IQ151ram[memoryPointer];  // Get byte to display
          uint8_t lineChar = videoROM[((videobyte & 0x7F) << 3) + (displayLine % 8)];  // Row in character
          if(videobyte & 0x80) {
                if(lineChar & 0x80) VGA_PIXELINROW(dest, pixelPointer+borderXSize) = bgcolor; pixelPointer++;
                if(lineChar & 0x40) VGA_PIXELINROW(dest, pixelPointer+borderXSize) = bgcolor; pixelPointer++;
                if(lineChar & 0x20) VGA_PIXELINROW(dest, pixelPointer+borderXSize) = bgcolor; pixelPointer++;
                if(lineChar & 0x10) VGA_PIXELINROW(dest, pixelPointer+borderXSize) = bgcolor; pixelPointer++;
                if(lineChar & 0x08) VGA_PIXELINROW(dest, pixelPointer+borderXSize) = bgcolor; pixelPointer++;
                if(lineChar & 0x04) VGA_PIXELINROW(dest, pixelPointer+borderXSize) = bgcolor; pixelPointer++;
                if(lineChar & 0x02) VGA_PIXELINROW(dest, pixelPointer+borderXSize) = bgcolor; pixelPointer++;
                if(lineChar & 0x01) VGA_PIXELINROW(dest, pixelPointer+borderXSize) = bgcolor; pixelPointer++;
          } else {   
                if(!(lineChar & 0x80)) VGA_PIXELINROW(dest, pixelPointer+borderXSize) = bgcolor; pixelPointer++;
                if(!(lineChar & 0x40)) VGA_PIXELINROW(dest, pixelPointer+borderXSize) = bgcolor; pixelPointer++;
                if(!(lineChar & 0x20)) VGA_PIXELINROW(dest, pixelPointer+borderXSize) = bgcolor; pixelPointer++;
                if(!(lineChar & 0x10)) VGA_PIXELINROW(dest, pixelPointer+borderXSize) = bgcolor; pixelPointer++;
                if(!(lineChar & 0x08)) VGA_PIXELINROW(dest, pixelPointer+borderXSize) = bgcolor; pixelPointer++;
                if(!(lineChar & 0x04)) VGA_PIXELINROW(dest, pixelPointer+borderXSize) = bgcolor; pixelPointer++;
                if(!(lineChar & 0x02)) VGA_PIXELINROW(dest, pixelPointer+borderXSize) = bgcolor; pixelPointer++;
                if(!(lineChar & 0x01)) VGA_PIXELINROW(dest, pixelPointer+borderXSize) = bgcolor; pixelPointer++;
          }
          memoryPointer++;
        } while (pixelPointer < 255);
    }
    // go to next scanline
    ++scanLine;
    dest += width;
  }
  if (scanLine == height) {
    // signal end of screen
    vTaskNotifyGiveFromISR(mainTaskHandle, NULL);
  }
}


// **************************************************************************************************
void setup()
{
  mainTaskHandle = xTaskGetCurrentTaskHandle();

  // Audio output
  pinMode(25, OUTPUT);    // Audio output
  pinMode(34, OUTPUT);    // Auxiliary output

  // Set VGA for IQ151 display grey monitor
  DisplayController.begin();
  DisplayController.setScanlinesPerCallBack(scanlinesPerCallback);
  DisplayController.setDrawScanlineCallback(drawScanline);
  DisplayController.setResolution(VGA_400x300_60Hz);
  PS2Controller.begin(PS2Preset::KeyboardPort0, KbdMode::GenerateVirtualKeys);
  fgcolor = DisplayController.createRawPixel(RGB222(3, 3, 3)); // white
  bgcolor = DisplayController.createRawPixel(RGB222(1, 1, 1)); // grey
  darkbgcolor = DisplayController.createRawPixel(RGB222(0, 0, 0)); // black
  width  = DisplayController.getScreenWidth();
  height = DisplayController.getScreenHeight();

  // Set CPU bus functions and start it
  m_i8080.setCallbacks(auxPoint, readByte, writeByte, readWord, writeWord, readIO, writeIO); 
  m_i8080.reset();
  for (int i = 0; i < 8; i++) { keyboardInL[i]=0xFF; keyboardInR[i]=0xFF; }

  // Set function pro Keyboard processing
  PS2Controller.keyboard()->onVirtualKey = [&](VirtualKey * vk, bool keyDown) {
      if (keyDown) {
        procesKeyDown(*vk);
    } else procesKeyUp(*vk);
  };
}

int storePC;    // Stack for save PC during interrupt

// **************************************************************************************************
// **************************************************************************************************
void loop()
{
  static int numCycles;
  numCycles = 0;
  while(numCycles < 30000) // approx. 30000 cycles per 16.6 milisec (60 Hz VGA)
  {
    for(int i=0; i<8; i++) { digitalWrite(34, HIGH); digitalWrite(34, LOW); } // delay for better audio
    numCycles += m_i8080.step();
  }
  // Interrupt services - "Break key" or "50 Hz timer"
  if(BreakFlag) { BreakFlag=false; m_i8080.setPC(0xFFF8); } 
  else {
    storePC = m_i8080.getPC();
    m_i8080.setPC(0xFFFB);
    do m_i8080.step(); while (m_i8080.getPC() != 0xFFFE);
    m_i8080.setPC(storePC);
    }

  // wait for vertical sync
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}
