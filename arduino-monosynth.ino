/*  Digital MIDI monophonic synth
 *
 *  Jan Tuomi 2016
 *  This project is in the public domain.
 *  Based on a public domain project by Tim Barrass, 2013.
 *  
 *  Connections:
 *    PIN 9       - AUDIO OUT
 *    RX          - MIDI SIGNAL IN
 *    PIN 10      - WAVEFORM TOGGLE BUTTON
 *    ANALOG 0    - DETUNE
 *    ANALOG 1    - ATTACK
 *    ANALOG 2	  - DECAY
 *    ANALOG 3    - SUSTAIN
 *    ANALOG 4    - RELEASE
 *    ANALOG 5    - VIBRATO AMP
 */

#include <MIDI.h>
#include <MozziGuts.h>
#include <Oscil.h> // oscillator template
#include <Line.h> // for envelope
#include <mozzi_midi.h>
#include <ADSR.h>
#include <mozzi_fixmath.h>
#include <Portamento.h>

/* Include waveform tables for oscillators */
#include <tables/cos512_int8.h> 
#include <tables/square_analogue512_int8.h>
#include <tables/triangle_analogue512_int8.h>
#include <tables/saw_analogue512_int8.h> 

// use #define for CONTROL_RATE, not a constant
#define CONTROL_RATE 256 // powers of 2 please

//  Mozzi example uses COS waves for carrier and modulator
//  Shit gets brutal very fast if you use a saw/square carrier
//  Shit gets subtly more brutal if you use smaller wavetables (fewer samples per cycle)

/* Use an array of three oscillators
 * The first will get double the amplitude */
Oscil<SAW_ANALOGUE512_NUM_CELLS, AUDIO_RATE> oscils[3] = 
  {(SAW_ANALOGUE512_DATA), (SAW_ANALOGUE512_DATA), (SAW_ANALOGUE512_DATA)};

/* Add a modulation and LFO oscillator */
Oscil<COS512_NUM_CELLS, AUDIO_RATE> oscModulator(COS512_DATA);
Oscil<COS512_NUM_CELLS, AUDIO_RATE> oscLFO(COS512_DATA);

/* Keep track of the currently selected waveform with an enum */
enum TableType {
  Cosine,
  Square,
  Saw,
  Triangle
} currentTable;

/* Store table data in a table for nice access */
const int8_t *tables[4] =
  { COS512_DATA, SQUARE_ANALOGUE512_DATA, SAW_ANALOGUE512_DATA, TRIANGLE_ANALOGUE512_DATA };

/* ADSR envelope for tuning the tone */
ADSR <CONTROL_RATE, AUDIO_RATE> envelope;
Portamento <CONTROL_RATE>aPortamento;

MIDI_CREATE_DEFAULT_INSTANCE();

/* Pin constants */

/* LED is used for waveform toggle visualisation */
#define LED 13

/* Table toggle button for changing waveform */
#define TABLE_TOGGLE 10
const uint8_t ANALOG_INPUTS[]
  = {0, 1, 2, 3, 4, 5};

// workaround for a idle buzzing bug
int sustainKillTimer = 0;

float carrierFreq = 10.f;
float modFreq = 10.f;
float modDepth = 0.5;
float amplitude = 0.f;
float modOffset = 1;
byte lastnote = 0;
//int portSpeed = 100;
byte detuneCents = 0;

int analog_values[6] = { 0 };

int tableToggleTimer = 0;
int TABLE_TOGGLE_TIMER_MAX = 100;

float detuneCoefficients[] = {1, 1};

float modOffsets[] = {
  4, 3.5, 3, 2.5,
  2, 1.5, 1, 0.6666667,
  0.5, 0.4, 0.3333333, 0.2857,
  0.25, 0, 0, 0,
  0, 0, 0
}; // freq ratios corresponding to DP's preferred intervals of 7, 12, 7, 19, 24, 0, 12, -12, etc

int attackTime, decayTime, releaseTime;
int sustainLevel;
float vibratoAmp = 0.f;

/* Maximum value for int */
const int SUSTAIN_TIME = (~0 >> 1);

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
  sustainLevel = 255;
  releaseTime = 345;
  
  envelope.setTimes(attackTime, decayTime, SUSTAIN_TIME, releaseTime); // 20000 is so the note will sustain 20 seconds unless a noteOff comes
  envelope.setSustainLevel(255);
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
  /* Read digital inputs
   *  i.e. waveform switch button
   */
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

  /* Read analog inputs */
  unsigned i;
  for (i = 0; i < sizeof(ANALOG_INPUTS) / sizeof(ANALOG_INPUTS[0]); i++) {
    analog_values[i] = mozziAnalogRead(i);
  }

  /* Map read values to corresponding variables */
  updateDetune(analog_values[0]);
  updateAttack(analog_values[1]);
  updateDecay(analog_values[2]);
  updateSustain(analog_values[3]);
  updateRelease(analog_values[4]);
  updateVibrato(analog_values[5]);
}

void updateDetune(int analog) {
  detuneCents = map(analog, 0, 1024, 0, 100);
  detuneCoefficients[0] = 1 + 0.0005946 * detuneCents;
  detuneCoefficients[1] = 1 - 0.0005946 * detuneCents;
}

void updateAttack(int analog) {
  attackTime = map(analog, 0, 1024, 0, 1024);
  envelope.setAttackTime(attackTime);
}

void updateDecay(int analog) {
  decayTime = map(analog, 0, 1024, 0, 1024);
  envelope.setDecayTime(decayTime);
}

void updateSustain(int analog) {
  sustainLevel = map(analog, 0, 1024, 0, 255);
  envelope.setSustainLevel(sustainLevel);
}

void updateRelease(int analog) {
  sustainTime = map(analog, 0, 1024, 0, 1024);
  envelope.setReleaseTime(releaseTime);
}

void updateVibrato(int analog) {
  vibratoAmp = ((float) map(analog, 0, 1024, 0, 100) ) / 100.f;
}

void updateControl()
{
  MIDI.read();
  envelope.update();
  carrierFreq = mtof(lastnote);
  modFreq = carrierFreq * modOffset;

   // update oscil types
  readPotsAndUpdate();
  
  // set carrier frequencies
  oscils[0].setFreq(carrierFreq);
  oscils[1].setFreq(carrierFreq * detuneCoefficients[0]);
  oscils[2].setFreq(carrierFreq * detuneCoefficients[1]);
  
  oscModulator.setFreq(modFreq);
}

int updateAudio()
{
  /* Sum together the signals from the 3 oscillators */
  int carrierSum = 0;
  for (int i = 0; i < sizeof(oscils)/sizeof(oscils[0]); i++) {
    /* The carrier signal gets double the amplitude */
    if (i == 0)
      carrierSum += 1.5f * (oscils[i].next() >> 3);
    else
      carrierSum += 0.75f * (oscils[i].next() >> 3);
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


