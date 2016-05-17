/*  Example of a sound being triggered by MIDI input.
 *
 *  Demonstrates playing notes with Mozzi in response to MIDI input,
 *  using the standard Arduino MIDI library:
 *  http://playground.arduino.cc/Main/MIDILibrary
 *
 *  Mozzi help/discussion/announcements:
 *  https://groups.google.com/forum/#!forum/mozzi-users
 *
 *  Tim Barrass 2013.
 *  This example code is in the public domain.
 *
 *  sgreen - modified to use standard Arduino midi library, added saw wave, lowpass filter
 *  Audio output from pin 9 (pwm)
 *  Midi plug pin 2 (centre) to Arduino gnd, pin 5 to RX (0)
 *  http://www.philrees.co.uk/midiplug.htm
 *
 * dgreen - nutty portamento added!
 */

#include <MIDI.h>
#include <MozziGuts.h>
#include <Oscil.h> // oscillator template
#include <Line.h> // for envelope
#include <mozzi_midi.h>
#include <ADSR.h>
#include <mozzi_fixmath.h>
#include <Portamento.h>
#include <tables/cos512_int8.h> // table for Oscils to play
#include <tables/square_analogue512_int8.h>
#include <tables/triangle_analogue512_int8.h>
#include <tables/saw_analogue512_int8.h> // table for Oscils to play

// use #define for CONTROL_RATE, not a constant
#define CONTROL_RATE 256 // powers of 2 please

//  Mozzi example uses COS waves for carrier and modulator
//  Shit gets brutal very fast if you use a saw/square carrier
//  Shit gets subtly more brutal if you use smaller wavetables (fewer samples per cycle)
Oscil<SAW_ANALOGUE512_NUM_CELLS, AUDIO_RATE> oscils[3] = 
  {(SAW_ANALOGUE512_DATA), (SAW_ANALOGUE512_DATA), (SAW_ANALOGUE512_DATA)};

Oscil<COS512_NUM_CELLS, AUDIO_RATE> oscModulator(COS512_DATA);
Oscil<COS512_NUM_CELLS, AUDIO_RATE> oscLFO(COS512_DATA);

enum TableType {
  Cosine,
  Square,
  Saw,
  Triangle
} currentTable;

const int8_t *tables[4] =
  { COS512_DATA, SQUARE_ANALOGUE512_DATA, SAW_ANALOGUE512_DATA, TRIANGLE_ANALOGUE512_DATA };

// envelope generator
ADSR <CONTROL_RATE, AUDIO_RATE> envelope;
Portamento <CONTROL_RATE>aPortamento;

MIDI_CREATE_DEFAULT_INSTANCE();

#define LED 13 // to see if MIDI is being recieved
#define TABLE_TOGGLE 10

// workaround for a idle buzzing bug
int sustainKillTimer = 0;

unsigned long vibrato = 0;
float carrierFreq = 10.f;
float modFreq = 10.f;
float modDepth = 0.5;
float amplitude = 0.f;
float modOffset = 1;
byte lastnote = 0;
//int portSpeed = 100;
byte detuneCents = 0;

int tableToggleTimer = 0;
int TABLE_TOGGLE_TIMER_MAX = 100;

float detuneCoefficient1 = 1;
float detuneCoefficient2 = 1;

float modOffsets[] = {
  4, 3.5, 3, 2.5,
  2, 1.5, 1, 0.6666667,
  0.5, 0.4, 0.3333333, 0.2857,
  0.25, 0, 0, 0,
  0, 0, 0
}; // freq ratios corresponding to DP's preferred intervals of 7, 12, 7, 19, 24, 0, 12, -12, etc

int attackTime, decayTime, sustainTime, releaseTime;

void setup()
{
  pinMode(LED, OUTPUT);
  pinMode(TABLE_TOGGLE, INPUT);
  
  MIDI.begin(MIDI_CHANNEL_OMNI);
  MIDI.setHandleNoteOn(HandleNoteOn);
  MIDI.setHandleNoteOff(HandleNoteOff);
  MIDI.setHandleControlChange(HandleControlChange);
  MIDI.setHandlePitchBend(HandlePitchBend);
  MIDI.setHandleProgramChange(HandleProgramChange);  

  envelope.setADLevels(255, 174);

  attackTime = 188;
  decayTime = 345;
  sustainTime = 65000000; // long enough
  releaseTime = 345;
  
  envelope.setTimes(attackTime, decayTime, sustainTime, releaseTime); // 20000 is so the note will sustain 20 seconds unless a noteOff comes
  envelope.setSustainLevel(255);
  //aPortamento.setTime(50u);
  oscLFO.setFreq(10); // default frequency
  
  startMozzi(CONTROL_RATE); 
}


void HandleProgramChange (byte channel, byte number)
{
  //  Use MIDI Program Change to select an interval between carrier and modulator oscillator
  modOffset = modOffsets[number % 15];
}

void HandleNoteOn(byte channel, byte note, byte velocity)
{ 
  //aPortamento.start((byte)(((int) note) - 5)); 

  lastnote = note;
  envelope.noteOn();
  oscLFO.setPhase(0);
  oscModulator.setPhase(0);
  sustainKillTimer = -1;
  digitalWrite(LED, HIGH);
}

void HandleNoteOff(byte channel, byte note, byte velocity)
{
  if (note == lastnote)
  {
    envelope.noteOff();
    sustainKillTimer = releaseTime;
    digitalWrite(LED, LOW);
  }
}

void HandlePitchBend (byte channel, int bend)
{
  float shifted = float ((bend + 8500) / 2048.f) + 0.1f;  
  oscLFO.setFreq(shifted);
  
}

void HandleControlChange (byte channel, byte number, byte value)
{
  if(number == 1)
  {
    float divided = float(value / 46.f);
    modDepth = (divided * divided);
    if (modDepth > 5)
    {
      modDepth = 5;
    }
    if (modDepth < 0.2)
    {
      modDepth = 0;
    }
  }
}

void setTables(const int8_t *TABLE_NAME) {
  for (int i = 0; i < sizeof(oscils)/sizeof(oscils[0]); i++) {
    oscils[i].setTable(TABLE_NAME);
  }
}

void readPotsAndUpdate() {
  if (tableToggleTimer > TABLE_TOGGLE_TIMER_MAX) {
    byte toggled = digitalRead(TABLE_TOGGLE);
    digitalWrite(LED, toggled);
    if (toggled) {
      currentTable = (TableType)(((byte)currentTable + 1) % 4);
      setTables(tables[(unsigned) currentTable]);
      tableToggleTimer = 0;
    }
  } else {
    tableToggleTimer++;
  }
  
  int pot1 = mozziAnalogRead(1);
  detuneCents = map(pot1, 0, 1024, 0, 100);
}

void updateControl()
{
  MIDI.read();
  envelope.update();
  carrierFreq = mtof(lastnote);
  modFreq = carrierFreq * modOffset;

   // update oscil types
  readPotsAndUpdate();
  
  detuneCoefficient1 = 1 + 0.0005946 * detuneCents;
  detuneCoefficient2 = 1 - 0.0005946 * detuneCents;

  /*
  byte toggled = digitalRead(TABLE_TOGGLE);
  digitalWrite(LED, toggled);
  */
  
  // set carrier frequencies
  oscils[0].setFreq(carrierFreq);
  oscils[1].setFreq(carrierFreq * detuneCoefficient1);
  oscils[2].setFreq(carrierFreq * detuneCoefficient2);
  
  oscModulator.setFreq(modFreq);
}

int updateAudio()
{
  vibrato = (unsigned long) (oscLFO.next() * oscModulator.next()) >> 7;
  vibrato *= (unsigned long) (carrierFreq * modDepth) >> 3;
  //int carrierSum = (oscCarrierMaster.phMod(vibrato) >> 3) + (oscCarrierSlave1.phMod(vibrato) >> 3) + (oscCarrierSlave2.phMod(vibrato) >> 3);

  int carrierSum = 0;
  for (int i = 0; i < sizeof(oscils)/sizeof(oscils[0]); i++) {
    carrierSum += (oscils[i].next() >> 3);
  }

  int total = (carrierSum * (envelope.next() >> 1)) >> 8;
  if (abs(total) < 5 || sustainKillTimer == 0)
    total = 0;
    
  if (sustainKillTimer > 0) {
    sustainKillTimer--;
  }
    
  return total;
  
}

void loop()
{
  audioHook();
} 


