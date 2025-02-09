#include <MIDIUSB.h>
#include <FastLED.h>

// NUMBER OF FOOT SWITCHES AND LEDS
const byte NUM_BUTTONS = 8; // 6 internal buttons + 2 external
const byte NUM_LEDS = 7;    // 0 is the indicator LED / 1-7 are the button LEDs

// HARDWARE PINS
// 'Serial1' Tx pin for DIN Midi Out = 1
const byte BUTTON_PINS[NUM_BUTTONS] = {3, 4, 5, 6, 7, 8, A1, A0}; // 6 foot switch pins + 2 external
const byte PIN_PROG1 = 14;                                        // 3-program selector switch PINs
const byte PIN_PROG2 = 15;                                        // 3-program selector switch PINs
const byte PIN_POTI = A3;                                         // (analog) EXPR. Pedal (Read on RING)
const byte LEDS_DATA_PIN = 2;                                     // 7x fastleds (WS2812B)

// TYPE DEFINITIONS
struct rxMidi // Stores the latest received midi data
{
  byte usbHeader;
  byte channel;
  byte type;
  byte pitch;
  byte velocity;
};

enum midiMessage // Accepted types of Midi Messages
{
  NOTE,
  NOTE_OFF,
  CC,
  PC, // 0-127 (equals Midi Programs 1-128)
  START,
  STOP,
  CONT
};

struct program // Store Program settings
{
  byte colorHue;                  // color of the LEDs in that program (0-255 or HUE_*)
  byte expressionCC;              // expression pedal control value to send on (0-127)
  byte expressionChannel;         // expression pedal channel (0-15)
  byte values[NUM_BUTTONS];       // pitch or control value to send on (0-127)
  midiMessage types[NUM_BUTTONS]; // type of midi message to send
  bool toggle[NUM_BUTTONS];       // toggle (1) or momentary (0) function
  byte channels[NUM_BUTTONS];     // channel to send on (0-15)
};

// ////////////////////////////////////////////////////////////////////////////////////
// ////////////////////////////////////////////////////////////////////////////////////
// PROGRAM SETTINGS
// ////////////////////////////////////////////////////////////////////////////////////

// Button Info: First 6 values for internal foot switches and last 2 for external

const program PROG0 = {
    .colorHue = HUE_GREEN,
    .expressionCC = 11,
    .expressionChannel = 0,
    .values = {88, 89, 91, 87, 0, 85, 100, 101},
    .types = {CC, CC, CC, CC, PC, CC, CC, CC},
    .toggle = {0, 0, 0, 0, 0, 0, 0, 0},
    .channels = {0, 0, 0, 0, 0, 0, 0, 0},
};

const program PROG1 = {
    .colorHue = HUE_RED,
    .expressionCC = 11,
    .expressionChannel = 8,
    .values = {24, 25, 26, 27, 28, 29, 30, 31},
    .types = {NOTE, NOTE, NOTE, NOTE, NOTE, NOTE, NOTE, NOTE},
    .toggle = {0, 0, 0, 0, 0, 0, 0, 0},
    .channels = {8, 8, 8, 8, 8, 8, 8, 8},
};

const program PROG2 = {
    .colorHue = HUE_BLUE,
    .expressionCC = 11,
    .expressionChannel = 9,
    .values = {0, 1, 2, 3, 4, 5, 6, 7},
    .types = {NOTE, NOTE, NOTE, NOTE, NOTE, NOTE, NOTE, NOTE},
    .toggle = {0, 0, 0, 0, 0, 0, 0, 0},
    .channels = {9, 9, 9, 9, 9, 9, 9, 9},
};

const byte NUM_PROGRAMS = 3;
const program PROGRAMS[NUM_PROGRAMS] = {PROG0, PROG1, PROG2};

// EXPRESSION PEDAL SETTINGS
const unsigned int ANALOG_MIN = 49;
const unsigned int ANALOG_MAX = 1020;
const unsigned int TIMEOUT = 200;      // Amount of time the potentiometer will be read after it exceeds the varThreshold
const unsigned int VAR_THRESHOLD = 16; // Threshold for the potentiometer signal variation

// FOOT SWITCH SETTINGS
const unsigned int DEBOUNCE_DELAY = 50; // debounce time in ms; increase if the output flickers

// ////////////////////////////////////////////////////////////////////////////////////
// ////////////////////////////////////////////////////////////////////////////////////

// ////////////////////////////////////////////////////////////////////////////////////
// GLOBAL MUTABLE VARIABLES

// Holds LEDS state
CRGB leds[NUM_LEDS] = {};
bool lastLedState[NUM_LEDS] = {};

// stores currently selected program
byte currentProg = 0x00;
byte lastProgPin1State = 0xFF;
byte lastProgPin2State = 0xFF;

// stores current on/off state for toggles for all 3 programs
bool currentlyOnStates[NUM_PROGRAMS][NUM_BUTTONS] = {};

// store foot switch state and time
byte buttonPreviousState[NUM_BUTTONS] = {};       // stores the buttons prev values
unsigned long lastDebounceTime[NUM_BUTTONS] = {}; // the last time the button pins were toggled

// Expression pedal state, time and mid value
int potPreviousState = 0;
unsigned long potPreviousTime = 0;
bool potStillMoving = true;
byte exprPreviousMidiValue = 0xFF;

// counts received midi clock pulses
byte midiClockCounter = 0x00;

/////////////////////////////////////////////////////////////////////
/////////////////// !! SETUP !! /////////////////////////////////////
void setup()
{
  // IFNotDEFined, because code generates an error squiggle in VSCode with the
  // current arduino extension even if there is no problem with 'Serial1'
#ifndef __INTELLISENSE__
  Serial1.begin(31250); // Set MIDI baud rate
#endif
  // Serial.begin(9600); // for debugging

  pinMode(PIN_PROG1, INPUT_PULLUP); // program selector pin1
  pinMode(PIN_PROG2, INPUT_PULLUP); // program selector pin2
  pinMode(PIN_POTI, INPUT_PULLUP);  // expr. pedal potentiometer pin (RING)
  for (int i = 0; i < NUM_BUTTONS; i++)
  {
    pinMode(BUTTON_PINS[i], INPUT_PULLUP); // foot switch pins
  }

  FastLED.addLeds<WS2812B, LEDS_DATA_PIN>(leds, NUM_LEDS); // init LEDs
  FastLED.setBrightness(8);                                // set Brightness (0-255)
  FastLED.clear();                                         // all LEDs off
  FastLED.show();                                          // refreshLed
}

//////////////////////////////////////////////////////////////////
//////////// !! LOOP !! //////////////////////////////////////////
void loop()
{
  updateProgram();

  sendFootSwitchMidi();

  sendExpressionPedalMidi();

  rxMidi refreshedRxMidi = receiveUsbMidi();

  updateToggleState(refreshedRxMidi);

  updateLeds(refreshedRxMidi);

  // serialDebug(refreshedRxMidi);
}
//////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////

// CHECKPROGRAM
void updateProgram()
{
  byte progPin1State = digitalRead(PIN_PROG1);
  byte progPin2State = digitalRead(PIN_PROG2);

  if (progPin1State != lastProgPin1State || progPin2State != lastProgPin2State)
  {
    if (progPin1State == 1 && progPin2State == 0)
      currentProg = 0;
    else if (progPin1State == 1 && progPin2State == 1)
      currentProg = 1;
    else if (progPin1State == 0 && progPin2State == 1)
      currentProg = 2;

    leds[0] = CHSV(PROGRAMS[currentProg].colorHue, 255, 255); // change PROG indicator led colour depending on program
    FastLED.show();

    lastProgPin1State = progPin1State;
    lastProgPin2State = progPin2State;
  }
}

// BUTTONS
void sendFootSwitchMidi()
{
  for (int i = 0; i < NUM_BUTTONS; i++)
  {
    if ((millis() - lastDebounceTime[i]) > DEBOUNCE_DELAY)
    {
      byte buttonCurrentState = digitalRead(BUTTON_PINS[i]);

      if (buttonPreviousState[i] != buttonCurrentState)
      {
        lastDebounceTime[i] = millis();

        if (buttonCurrentState == LOW) // BUTTON PUSHED
        {
          byte sendValue = 127;

          if (PROGRAMS[currentProg].toggle[i])
          {
            currentlyOnStates[currentProg][i] = !currentlyOnStates[currentProg][i];
            sendValue = currentlyOnStates[currentProg][i] ? 127 : 0;
          }

          sendMidi(PROGRAMS[currentProg].types[i], PROGRAMS[currentProg].channels[i], PROGRAMS[currentProg].values[i], sendValue);
        }
        else if (PROGRAMS[currentProg].toggle[i] == false) // BUTTON RELEASED
        {
          if (PROGRAMS[currentProg].types[i] == midiMessage::NOTE)
          {
            sendMidi(midiMessage::NOTE_OFF, PROGRAMS[currentProg].channels[i], PROGRAMS[currentProg].values[i], 0);
          }
          if (PROGRAMS[currentProg].types[i] == midiMessage::CC)
          {
            sendMidi(midiMessage::CC, PROGRAMS[currentProg].channels[i], PROGRAMS[currentProg].values[i], 0);
          }
        }

        buttonPreviousState[i] = buttonCurrentState;
      }
    }
  }
}

/////////////////////////////////////////////
// EXPRESSION PEDAL
void sendExpressionPedalMidi()
{
  int potCurrentState = analogRead(PIN_POTI);
  byte exprCurrentMidiValue = map(potCurrentState, ANALOG_MIN, ANALOG_MAX, 0, 127);

  int potChange = abs(potCurrentState - potPreviousState);

  if (potChange > VAR_THRESHOLD)
  { // Opens the gate if the potentiometer variation is greater than the threshold
    potPreviousTime = millis();
  }

  // If the potTimer is less than the maximum allowed time it means that the potentiometer is still moving
  potStillMoving = millis() - potPreviousTime < TIMEOUT;

  if (potStillMoving)
  { // If the potentiometer is still moving, send the control change (if necessary)
    if (exprPreviousMidiValue != exprCurrentMidiValue)
    {
      sendMidi(midiMessage::CC, PROGRAMS[currentProg].expressionChannel, PROGRAMS[currentProg].expressionCC, exprCurrentMidiValue);

      potPreviousState = potCurrentState;
      exprPreviousMidiValue = exprCurrentMidiValue;
    }
  }
}

/////////////////////////////////////////////////
///////////// REFRESH RECEIVED MIDI DATA BUFFER
rxMidi receiveUsbMidi()
{
  midiEventPacket_t rx = MidiUSB.read(); // get latest received Midi from USB

  rxMidi rxMidi = {
      .usbHeader = rx.header,
      .channel = rx.byte1 & B00001111, // Separated rx.byte1 to get Channel
      .type = rx.byte1 >> 4,           // Separated rx.byte1 to get Type
      .pitch = rx.byte2,
      .velocity = rx.byte3,
  };

  return rxMidi;
}

void updateToggleState(rxMidi rxMidi)
{
  if (rxMidi.type == 0x9 || rxMidi.type == 0xB || rxMidi.type == 0x8)
  {
    for (byte i = 0; i < NUM_BUTTONS; i++)
    {
      if (rxMidi.pitch != PROGRAMS[currentProg].values[i])
        continue;

      if (rxMidi.type == 0x8 || rxMidi.velocity == 0)
      {
        currentlyOnStates[currentProg][i] = false;
      }
      else
      {
        currentlyOnStates[currentProg][i] = true;
      }
    }
  }
}

void updateLeds(rxMidi rxMidi)
{
  //////////////////////////
  // CLOCK
  // receive midi clock (type 0xF / channels: 0xA=start; 0xC=stop; 0x8=pulse / 24 pulses per quarter note)

  bool refreshLED = false;

  if (rxMidi.type == 0xF)
  {
    if (rxMidi.channel == 0xA || rxMidi.channel == 0xC) // receive a start or stop
    {
      midiClockCounter = 0;
      refreshLED = true;
    }
    else if (rxMidi.channel == 0x8) // receive clock pulse
    {
      midiClockCounter++;
      if (midiClockCounter >= 24) // mid signal transmits 24 pulses per quarter note
      {
        midiClockCounter = 0;
      }
      refreshLED = true;
    }
  }

  if (refreshLED)
  {
    if (midiClockCounter == 0)
    {
      leds[0] = CHSV(PROGRAMS[currentProg].colorHue, 255, 255); // LED ON
      FastLED.show();
    }
    else if (midiClockCounter == 2) // length of each blink in midi pulses (default:2)
    {
      leds[0] = CHSV(0, 0, 0); // LED OFF
      FastLED.show();
    }
  }

  /////////////////////////////
  // check on/off state
  for (byte b = 0; b < NUM_LEDS; b++)
  {
    if (currentlyOnStates[currentProg][b] != lastLedState[b])
    {
      byte led = b + 1;

      if (currentlyOnStates[currentProg][b])
      {
        leds[led] = CHSV(PROGRAMS[currentProg].colorHue, 255, 255);
      }
      else
      {
        leds[led] = CHSV(0, 0, 0);
      }

      FastLED.show();
      lastLedState[b] = currentlyOnStates[currentProg][b];
    }
  }
}

void serialDebug(rxMidi rxMidi)
{
  // DEBUG raw rxMidi data
  if (rxMidi.usbHeader != 0)
  {
    Serial.print("USB-Header: ");
    Serial.print(rxMidi.usbHeader, HEX);

    Serial.print(" / Type: ");
    switch (rxMidi.type)
    {
    case 0x08:
      Serial.print("NoteOFF");
      break;
    case 0x09:
      Serial.print("NoteON");
      break;
    case 0x0B:
      Serial.print("CC");
      break;
    case 0x0C:
      Serial.print("PC");
      break;
    default:
      Serial.print("[");
      Serial.print(rxMidi.type);
      Serial.print("]");
      break;
    }

    Serial.print(" / Channel: ");
    Serial.print(rxMidi.channel);

    Serial.print(" / Pitch: ");
    Serial.print(rxMidi.pitch);

    Serial.print(" / Velocity: ");
    Serial.println(rxMidi.velocity);
  }
}

/////////////////////////////////////////////////////////////////////////////////////
// MIDI messages via serial bus

// Sends a midi signal on the serial bus
// cmd = message type and channel,
// 0xFF is out of the 7bit midi range and will not be sent
void midiSerial(byte cmd, byte pitch = 0xFF, byte velocity = 0xFF)
{
#ifndef __INTELLISENSE__
  Serial1.write(cmd);
#endif
  if (pitch <= 0x7F)
  {
#ifndef __INTELLISENSE__
    Serial1.write(pitch);
#endif
  }
  if (velocity <= 0x7F)
  {
#ifndef __INTELLISENSE__
    Serial1.write(velocity);
#endif
  }
}

/////////////////////////////////////////////////////////////////////////////////////
// Functions for sending Midi Messages

void midiStart(byte channel)
{
  midiEventPacket_t ccPacket = {0x0F, 0xFA | channel, 0x00, 0x00};
  MidiUSB.sendMIDI(ccPacket); // queue message to buffer
  midiSerial(0xFA | channel);
}

void midiStop(byte channel)
{
  midiEventPacket_t ccPacket = {0x0F, 0xFC | channel, 0x00, 0x00};
  MidiUSB.sendMIDI(ccPacket); // queue message to buffer
  midiSerial(0xFC | channel);
}

void midiCont(byte channel)
{
  midiEventPacket_t ccPacket = {0x0F, 0xFB | channel, 0x00, 0x00};
  MidiUSB.sendMIDI(ccPacket); // queue message to buffer
  midiSerial(0xFB | channel);
}

void noteOn(byte channel, byte pitch, byte velocity)
{
  midiEventPacket_t noteOnPacket = {0x09, 0x90 | channel, pitch, velocity};
  MidiUSB.sendMIDI(noteOnPacket); // queue message to buffer
  midiSerial(0x90 | channel, pitch, velocity);
}

void noteOff(byte channel, byte pitch)
{
  // midiEventPacket_t noteOffPacket = {0x08, 0x80 | channel, pitch, 0}; // note off message
  midiEventPacket_t noteOnPacket = {0x09, 0x90 | channel, pitch, 0}; // note on with velocity 0
  MidiUSB.sendMIDI(noteOnPacket);                                    // queue message to buffer
  // midiSerial(0x80 | channel, pitch, 0); // note off message
  midiSerial(0x90 | channel, pitch, 0); // note on with velocity 0
}

void controlChange(byte channel, byte control, byte value)
{
  midiEventPacket_t ccPacket = {0x0B, 0xB0 | channel, control, value};
  MidiUSB.sendMIDI(ccPacket); // queue message to buffer
  midiSerial(0xB0 | channel, control, value);
}

void programChange(byte channel, byte program)
{
  midiEventPacket_t ccPacket = {0x0C, 0xC0 | channel, program, 0x00};
  MidiUSB.sendMIDI(ccPacket); // queue message to buffer
  midiSerial(0xC0 | channel, program);
}

void sendMidi(midiMessage type, byte channel, byte val1, byte val2)
{
  switch (type)
  {
  case midiMessage::NOTE:
    noteOn(channel, val1, val2);
    break;
  case midiMessage::NOTE_OFF:
    noteOff(channel, val1);
    break;
  case midiMessage::CC:
    controlChange(channel, val1, val2);
    break;
  case midiMessage::PC:
    programChange(channel, val1);
    break;
  case midiMessage::START:
    midiStart(channel);
    break;
  case midiMessage::STOP:
    midiStop(channel);
    break;
  case midiMessage::CONT:
    midiCont(channel);
    break;
  }
  MidiUSB.flush(); // send midi buffer
};