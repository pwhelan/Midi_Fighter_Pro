// Main loop for the Midifighter Pro firmware
//
//   Copyright (C) 2009-2011 Robin Green
//
// rgreen 2010-10-27

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <avr/io.h>
#include <avr/wdt.h>
#include <avr/boot.h>
#include <avr/power.h>
#include <avr/pgmspace.h>
#include <avr/interrupt.h>
#include <util/delay.h>

#include <LUFA/Version.h>                 // Library Version Information
#include <LUFA/Drivers/USB/USB.h>         // USB Functionality
#include <LUFA/Drivers/USB/Class/MIDI.h>  // High level MIDI functionality

#include "modeldefs.h" // NOTE: include this first.

#include "key.h"
#include "led.h"
#include "spi.h"
#include "menu.h"
#include "midi.h"
#include "eeprom.h"
#include "random.h"
#include "selftest.h"
#include "constants.h"
#include "expansion.h"
#include "usb_descriptors.h"
#include "sysex.h"
#include "config.h"
#include "combo.h"
#include "jumptoboot.h"

// Forward Declarations --------------------------------------------------------

// Declare the MIDI function.
void MIDI_Task(void);

// The USB events handled by this program.
void EVENT_USB_Device_Connect(void);
void EVENT_USB_Device_Disconnect(void);
void EVENT_USB_Device_ConfigurationChanged(void);
void EVENT_USB_Device_UnhandledControlRequest(void);

static bool main_watchdog_flag = false;

// Helper functions ------------------------------------------------------------

uint8_t remap(uint8_t value, uint8_t from, uint8_t to, uint8_t lo, uint8_t hi)
{
    // Remap the values in the range [lo..hi] into the values [from..to]
    // e.g. remap the value in the range 3..124 into values 0..127 with a
    // dead zone at either end.

    //    d      e
    // 2  D2=64  E2=124   from, to
    // 3  D3=0   E3=127    lo, hi

    // =IF(A15 < $D$2 , $D$3 ,
    //     IF(A15 > $E$2, $E$3,
    //         $D$3 + FLOOR( FLOOR((A15-$D$2)*($E$3-$D$3),1) / floor($E$2-$D$2,1) ,1)))

    // IF(A15 < from)  lo
    // IF(A15 > to) hi
    // else lo +  (A15-from) * (hi-lo) ) / (to-from)

    if (value < from) return lo;
    if (value > to) return hi;
    uint16_t numer = (value - from) * (hi - lo);
    uint16_t denom = (to - from);
    return (uint8_t)(lo + (numer / denom));
}

// USB Tasks and Events --------------------------------------------------------

// We are in the process of enumerating but not yet ready to generate MIDI.
//
void EVENT_USB_Device_Connect(void)
{
    // Indicate that USB is enumerating.
    led_set_state(0x0002);
}

// The device is no longer connected to a host.
//
void EVENT_USB_Device_Disconnect(void)
{
    // Indicate that USB is disconnected.
    led_set_state(0x0001);
}

// Device has enumerated. Set up the Endpoints.
//
void EVENT_USB_Device_ConfigurationChanged(void)
{
    // Indicate that USB is now ready to use (followed by a short delay so
    // you can actually see it flash).
    led_set_state(0x0004);

    // Allow the LUFA MIDI Class drivers to configure the USB endpoints.
    if (!MIDI_Device_ConfigureEndpoints(g_midi_interface_info)) {
        // Setting up the endpoints failed, display the error state.
        led_set_state(0x0008);
    }

    // Success. Add a short delay so the final USB state LEDs can be seen
    // before the MIDI task takes over the LEDs.
    _delay_ms(40);
    led_set_state(0x0000);
	// Now we can enable the watchdog timer
	MCUSR &= ~(1 << WDRF);  // clear the watchdog reset flag
	wdt_enable(WDTO_120MS);
}

// Any other USB control command that we don't recognize is handled here.
//
void EVENT_USB_Device_UnhandledControlRequest(void)
{
    // Let the LUFA MIDI Class handle this request.
    MIDI_Device_ProcessControlRequest(g_midi_interface_info);
}

// ***************************
// This code is here to debug the LED freeze when connected but not being
// listened to under Windows.
// **************************
//
// bool My_Device_ReceiveEventPacket(
//         USB_ClassInfo_MIDI_Device_t* const MIDIInterfaceInfo,
//         MIDI_EventPacket_t* const Event)
// {
//     if (USB_DeviceState != DEVICE_STATE_Configured) {
//         led_set_state(0x0001);
//         return false;
//     }
//
//     Endpoint_SelectEndpoint(MIDIInterfaceInfo->Config.DataOUTEndpointNumber);
//
//     led_set_state(0x0002);
//
//     if (!(Endpoint_IsReadWriteAllowed())) {
//         led_set_state(0x0004);
//         return false;
//     }
//
//     Endpoint_Read_Stream_LE(Event,
//                             sizeof(MIDI_EventPacket_t),
//                             NO_STREAM_CALLBACK);
//
//     led_set_state(0x0008);
//
//     if (!(Endpoint_IsReadWriteAllowed())) {
//         led_set_state(0x0010);
//         Endpoint_ClearOUT();
//     }
//
//     led_set_state(0x0020);
//
//     return true;
// }


// The MIDI processing task.
//
// Read the buttons and expansion ports to generate MIDI notes. This routine
// is the heart of the MidiFighter.
//
void Midifighter_Task(void)
{
    // If the Midifighter is not completely enumerated by the USB Host,
    // don't go any further - no updating of LEDs, no reading from
    // endpoints, we wait for the USB to connect.
    if (USB_DeviceState != DEVICE_STATE_Configured) {
		// Set watchdog flag so main loop knows this section ran
		main_watchdog_flag = true;
        return;
    }

    // Overview
    // --------
    // The state of all the active notes is kept in an array of bytes
    // recording the most recent velocity of the note. A nonzero velocity is
    // a NoteOn and a zero velocity is a NoteOff. We update the keystate
    // from the outside world first, from the keyboard second, from the
    // expansion port third and generate LEDs from the resulting table at
    // the end.
    //
    // Midi Map
    // --------
    // In normal mode only 16 notes are being tracked, as well as the
    // digital expansion ports, plus two notes for each analog port for the
    // smart filters:
    //
    //     2  2  3  3  <- analog 2,3 = 104 .. 107
    //     0  0  1  1  <- analog 0,1 = 100 .. 103
    //
    //     .  .  .  .  <- bank 0 = 48 .. 52
    //     .  .  .  .  <- bank 0 = 44 .. 47
    //     .  .  .  .  <- bank 0 = 40 .. 43
    //     .  .  .  .  <- bank 0 = 36 .. 39
    //
    //     D  D  D  D  <- digital = 4 .. 7
    //
    //
    // In 4banks Internal mode, the top 4 buttons are used as bank
    // selection keys so we are tracking four banks of 12 notes plus the
    // digital and analog notes.
    //
    //     .  .  .  .  <- 124 .. 127
    //     .  .  .  .  <- 120 .. 123
    //     .  .  .  .  <- 116 .. 119
    //     .  .  .  .  <- 108 .. 115
    //     2  2  3  3  <- analog 2,3 = 104 .. 107
    //     0  0  1  1  <- analog 0,1 = 100 .. 103
    //     .  .  .  .  <- bank 3 = 96 .. 99
    //     .  .  .  .  <- bank 3 = 92 .. 95
    //     .  .  .  .  <- bank 3 = 88 .. 91
    //     .  .  .  .  <- bank 3 = 84 .. 87
    //     @  @  @  @  <- bank 3 = 80 .. 83
    //     @  @  @  @  <- bank 3 = 76 .. 79
    //     @  @  @  @  <- bank 3 = 72 .. 75
    //     #  #  #  #  <- bank 2 = 68 .. 71
    //     #  #  #  #  <- bank 2 = 64 .. 67
    //     #  #  #  #  <- bank 2 = 60 .. 63
    //     @  @  @  @  <- bank 1 = 56 .. 59
    //     @  @  @  @  <- bank 1 = 52 .. 55
    //     @  @  @  @  <- bank 1 = 48 .. 51
    //     #  #  #  #  <- bank 0 = 44 .. 47
    //     #  #  #  #  <- bank 0 = 40 .. 43
    //     #  #  #  #  <- bank 0 = 36 .. 39
    //     .  .  .  .  <- 32 .. 35
    //     .  .  .  .  <- 28 .. 31
    //     .  .  .  .  <- 24 .. 27
    //     .  .  .  .  <- 20 .. 23
    //     .  .  .  .  <- 16 .. 19
    //     .  .  .  .  <- 12 .. 15
    //     .  .  .  .  <- 08 .. 11
    //     D  D  D  D  <- digital = 4 .. 7
    //     B  B  B  B  <- bank select keys 0..3
    //
    //
    // In 4banks External mode, the four digital pins are used as bank
    // select keys giving us four banks of 16 keys:
    //
    //     .  .  .  .  <- 124 .. 127
    //     .  .  .  .  <- 120 .. 123
    //     .  .  .  .  <- 116 .. 119
    //     .  .  .  .  <- 108 .. 115
    //     2  2  3  3  <- analog 2,3 = 104 .. 107
    //     0  0  1  1  <- analog 0,1 = 100 .. 103
    //     @  @  @  @  <- bank 3 = 96 .. 99
    //     @  @  @  @  <- bank 3 = 92 .. 95
    //     @  @  @  @  <- bank 3 = 88 .. 91
    //     @  @  @  @  <- bank 3 = 84 .. 87
    //     #  #  #  #  <- bank 2 = 80 .. 83
    //     #  #  #  #  <- bank 2 = 76 .. 79
    //     #  #  #  #  <- bank 2 = 72 .. 75
    //     #  #  #  #  <- bank 2 = 68 .. 71
    //     @  @  @  @  <- bank 1 = 64 .. 67
    //     @  @  @  @  <- bank 1 = 60 .. 63
    //     @  @  @  @  <- bank 1 = 56 .. 59
    //     @  @  @  @  <- bank 1 = 52 .. 55
    //     #  #  #  #  <- bank 0 = 48 .. 51
    //     #  #  #  #  <- bank 0 = 44 .. 47
    //     #  #  #  #  <- bank 0 = 40 .. 43
    //     #  #  #  #  <- bank 0 = 36 .. 39
    //     .  .  .  .  <- 32 .. 35
    //     .  .  .  .  <- 28 .. 31
    //     .  .  .  .  <- 24 .. 27
    //     .  .  .  .  <- 20 .. 23
    //     .  .  .  .  <- 16 .. 19
    //     .  .  .  .  <- 12 .. 15
    //     .  .  .  .  <- 08 .. 11
    //     D  D  D  D  <- digital = 04 .. 07
    //     B  B  B  B  <- bank select keys 00 .. 03
    //
    //
    // The Bank Select key events are sent whenever a bank select key is
    // pressed, regardless whether the key is on the digital port or on the
    // keypad.


	SysEx_t sysEx;
	uint8_t index = 0;


    // INPUT MIDI from USB -----------------------------------------------------

    // If there is data in the Endpoint for us to read, get a USB-MIDI
    // packet to process. Endpoint_IsReadWriteAllowed() returns true if
    // there is data remaining inside an OUT endpoint or if an IN endpoint
    // has space left to fill. The same function doing two jobs, confusing
    // but there you are.
    MIDI_EventPacket_t input_event;
    while (MIDI_Device_ReceiveEventPacket(g_midi_interface_info,
                                          &input_event)) {
        // Assuming all virtual MIDI cables are intended for us, ensure that
        // this event is being sent on our current MIDI channel.
        //
        // The lower 4-bits (".Command") of the USB_MIDI event packet tells
        // us what kind of data it contains, and whether to expect more data
        // in the same message. Commands are:
        //     0x0 = Reserved for Misc
        //     0x1 = Reserved for Cable events
        //     0x2 = 2-byte System Common
        //     0x3 = 3-byte System Common
        //     0x4 = 3-byte Sysex starts or continues
        //     0x5 = 1-byte System Common or Sysex ends
        //     0x6 = 2-byte Sysex ends
        //     0x7 = 3-byte Sysex ends
        //     0x8 = Note On
        //     0x9 = Note Off
        //     0xA = Poly KeyPress
        //     0xB = Control Change (CC)
        //     0xC = Program Change
        //     0xD = Channel Pressure
        //     0xE = PitchBend Change
        //     0xF = 1-byte message

        // System Real Time events don't have a channel, so we check for
        // them first.
        if (input_event.Command == 0xF) {
            if (input_event.Data1 == 0xF8) {
                // Clock event, increment the counter.
                g_led_groundfx_counter++;
            } else if (input_event.Data1 == 0xFA) {
                // Song Start, reset the counter.
                g_led_groundfx_counter = 0;
            } else if (input_event.Data1 == 0xFC) {
                // Song Stop event, reset the counter.
                g_led_groundfx_counter = 0;
            }
        }
		else if (input_event.Command == 0x4) { // Start or continue 3 bytes
			SYSEX_READ(1);
			SYSEX_READ(2);
			SYSEX_READ(3);
		}
		else if (input_event.Command == 0x5) { // End  1 byte
			SYSEX_READ(1);
			SYSEX_END();
		}
		else if (input_event.Command == 0x6) { // End 2 bytes
			SYSEX_READ(1);
			SYSEX_READ(2);
			SYSEX_END();
		}
		else if (input_event.Command == 0x7) { // End 3 bytes
			SYSEX_READ(1);
			SYSEX_READ(2);
			SYSEX_READ(3);
			SYSEX_END();
		}
		else {
			// Now we can check that the MIDI channel is the one we're payin
			// attention to before parsing the event.
			uint8_t channel = input_event.Data1 & 0x0f;
			if (channel == g_midi_channel) {
				// Check to see if we have a NoteOn or NoteOff event.
				switch (input_event.Command) {
				case 0x9 : {
						// A NoteOn event was found, so update the MIDI
						// keystate with the note velocity (which may be
						// zero).
						uint8_t note = input_event.Data2;
						uint8_t velocity = input_event.Data3;
						// record the note velocity in the MIDI note state
						g_midi_note_state[note] = velocity;
					}
					break;
				case 0x8 : {
						// A NoteOff event, so record a zero in the MIDI
						// keystate. Yes, a noteoff can have a "velocity",
						// but we're relying on the keystate to be zero when
						// we have a noteoff, otherwise the LEDs won't match
						// the state when we come to calculate them.
						uint8_t note = input_event.Data2;
						// record a zero note velocity in the MIDI note state
						g_midi_note_state[note] = 0;
					}
					break;
				}  // end switch on command
			} // end channel test
		}
    } // end while


    // OUTPUT events from the EXPANSION ports ----------------------------------

    // Generate MIDI events the digital input ports
    //
    exp_key_read();  // scan the debounce buffer.
    exp_key_calc();  // update the keyup/keydown variables.

    // Expansion port pins generate the MIDI notes 4 to 7.
    const uint8_t MIDI_DIGITAL_NOTE = 4;  // lowest digital note.

    // NOTE: enabling fourbanks external mode turns off digital note generation.
    if (g_key_fourbanks_mode != FOURBANKS_EXTERNAL) {
        uint8_t bit = 0x01;
        for(uint8_t i=0; i<4; ++i) {
            if (g_exp_key_down & bit) {
                // There's a key down, generate a NoteOn
                midi_stream_note(MIDI_DIGITAL_NOTE + i, true);
            }
            if (g_exp_key_up & bit) {
                // There's a key up, insert a NoteOff
                midi_stream_note(MIDI_DIGITAL_NOTE + i, false);
            }
            bit <<= 1;
        }
    }

    // Generate MIDI events for the four analog ports only if they've
    // changed their value since the last time we read them.

    // Read the full 10-bit value from each ADC channel. NOTE: We tried
    // averaging multiple reads of the ADC but found that the data was
    // good enough to use with only a single read.

    static uint16_t adc_value[NUM_ANALOG];

    // Read the full 10-bit value from each ADC channel. Take the
    // average of several samples to smooth out the noise.
    for (uint8_t i=0; i<NUM_ANALOG; ++i) {
        adc_value[i] = 0;
    }
    for (uint8_t samples = 0; samples < 4; ++samples) {
        for (uint8_t i=0; i<NUM_ANALOG; ++i) {
            adc_value[i] += exp_adc_read(i);
        }
    }
    for (uint8_t i=0; i<NUM_ANALOG; ++i) {
        adc_value[i] >>= 2;
    }
	
	// invert the sliders if necessary
	// must be performed before hysteresis otherwise
	// causes noise artifacts.
	if (!g_rotate_enable)
	{
	#ifdef INVERT_SLIDER_1
        adc_value[0] = 1024 - adc_value[0];
	#endif
	#ifdef INVERT_SLIDER_2
        adc_value[1] = 1024 - adc_value[1];
	#endif
	#ifdef INVERT_SLIDER_3
        adc_value[2] = 1024 - adc_value[2];
	#endif
	#ifdef INVERT_SLIDER_4
        adc_value[3] = 1024 - adc_value[3];
	#endif
	}	

    // Make sure any change in the value is due to
    // user action and not sampling noise. We do this by making sure the
    // value has changed by a minimum amount before we say it has
    // changed - essentially adding a small amount of Hysteresis into
    // the system.
    for (uint8_t i=0; i<NUM_ANALOG; ++i) {
        // Need a signed value for the difference
        int16_t difference = adc_value[i] - g_exp_analog_prev[i];
        // If the difference is less than four bits either way we
        // assume the difference was noise and the ADC was not changed.
        if (abs(difference) < 8) {
            adc_value[i] = g_exp_analog_prev[i];
        }
    }

    // Next, check the ADC values to see if they have changed.
    for (uint8_t i=0; i<NUM_ANALOG; ++i) {

        // Lose the bottom three bits of each 10-bit ADC value,
        // converting it to a 7-bit CC value.
        uint8_t value = (uint8_t)(adc_value[i] >> 3);
        uint8_t prev_value = (uint8_t)(g_exp_analog_prev[i] >> 3);

        // Compare the CC value to the previous one sent. If there has
        // been a change, generate the three new MIDI events.
        if (value != prev_value) {

            const uint8_t NOTEON_LOW = 3;
            const uint8_t NOTEON_HIGH = 127 - NOTEON_LOW;
            const uint8_t MIDI_ANALOG_NOTE = 100;
            const uint8_t MIDI_ANALOG_CC = 16;
            uint8_t cc_a = MIDI_ANALOG_CC + 2*i;
            uint8_t cc_b = MIDI_ANALOG_CC + 2*i + 1;
            uint8_t note_a = MIDI_ANALOG_NOTE + 2*i;
            uint8_t note_b = MIDI_ANALOG_NOTE + 2*i + 1;

            // New mapping style:
            //
            //   0  3             64           124 127
            //   |--|-------------|-------------|--|   - full range
            //
            //      |0=======================127|      - CC A
            //                    |0=========105|      - CC B
            //
            //   |__|on____________________________|   - note A
            //   |off___________________________|on|   - note B
            //      3                          124
            //
            if (value >= NOTEON_LOW && value <= NOTEON_HIGH) {
                // 1. Generate the default CC event.
                midi_stream_cc(cc_a, remap(value, NOTEON_LOW,NOTEON_HIGH, 0,127));

				if (g_device_mode == TRAKTOR)
				{
					// 2. If the value is in the range 50%-100%, output the
					// second CC range.
					static uint8_t second_cc_value = 0;
					if (value >= 64) {
						second_cc_value = remap(value, 64,NOTEON_HIGH, 0,105);
						midi_stream_cc(cc_b, second_cc_value);
					} else {
						// Make sure we zero the second CC value when we
						// enter the lower range.
						if (second_cc_value > 0) {
							second_cc_value = 0;
							midi_stream_cc(cc_b, second_cc_value);
						}
					}
				}				
            }

            // 3. Generate a Note event if we have just entered or left
            //    the top or bottom tick of the range. Values turn on as
            //    we leave the bottom or enter the top:
            //
            //   |off|on----------------------------| note A
            //   |off----------------------------|on| note B
            //
			if (g_device_mode == TRAKTOR)
			{
				if (value <= NOTEON_LOW && prev_value > NOTEON_LOW) {
					midi_stream_note(note_a, true);
					g_midi_note_state[note_a] = g_midi_velocity;
				} else if (value > NOTEON_LOW && prev_value <= NOTEON_LOW) {
					midi_stream_note(note_a, false);
					g_midi_note_state[note_a] = 0;
				} else if (value >= NOTEON_HIGH && prev_value < NOTEON_HIGH) {
					midi_stream_note(note_b, true);
					g_midi_note_state[note_b] = g_midi_velocity;
				} else if (value < NOTEON_HIGH && prev_value >= NOTEON_HIGH) {
					midi_stream_note(note_b, false);
					g_midi_note_state[note_b] = 0;
				}
			}			

            // Record the new ADC value for next time through.
            g_exp_analog_prev[i] = adc_value[i];
        }
    }

    // OUTPUT key presses ------------------------------------------------------

    key_read();  // Read the debounce buffer to generate a keystate.
    key_calc();  // Use the new keystate to update keydown/keyup state.

    // Setup the variables for Bank output based on the Fourbanks mode.
    uint16_t bank_keydown = 0;
    uint16_t bank_keyup = 0;
    uint16_t bank_keystate = 0;
    uint16_t keydown = 0;
    uint16_t keyup = 0;
    uint8_t keyoffset = 0;
    uint8_t keycount = 0;

    if (g_key_fourbanks_mode == FOURBANKS_OFF) {

        // Fourbanks Off
        // -------------
        // No bank keys to generate MIDI for.
        bank_keydown = 0;
        bank_keyup = 0;
        bank_keystate = 0;
        keydown = g_key_down;
        keyup = g_key_up;
        keyoffset = 0;
        keycount = 16;

        // Only bank zero is active.
        g_key_bank_selected = 0;

    } else if (g_key_fourbanks_mode == FOURBANKS_INTERNAL) {

        // Fourbanks Internal
        // ------------------
        // The top four keys control which bank we are reading. If any of
        // them are being activated we may need to swap the displayed bank.
        bank_keydown = g_key_down;
        bank_keyup = g_key_up;
        bank_keystate = g_key_state;
        keydown = g_key_down >> 4;
        keyup = g_key_up >> 4;
        keyoffset = 4;
        keycount = 12;

    } else if (g_key_fourbanks_mode == FOURBANKS_EXTERNAL) {

        // Fourbanks External
        // ------------------
        // In Fourbanks External mode, g_exp_digital_read has been disabled.
        // All 16 keys are banked with the bank being selected by keys on
        // the Digital Expansion ports.
        bank_keydown = g_exp_key_down;
        bank_keyup = g_exp_key_up;
        bank_keystate = g_exp_key_state;
        keydown = g_key_down;
        keyup = g_key_up;
        keyoffset = 0;
        keycount = 16;

    } // fourbanks setup

    // Update the active bank
    // ----------------------
    if (bank_keydown & 0x000f) {
        // The bank selected will be the most recently pressed key. If
        // multiple keys are pressed at the same instant, choose the
        // leftmost key.
        uint8_t bank_bit = 1;
        uint8_t new_bank = 0;
        while (!(bank_keydown & bank_bit) && new_bank < 4) {
            bank_bit <<= 1;
            ++new_bank;
        }
        // Force a NoteOff if a new bank has been selected but the
        // previous bank is still depressed.
        if ((g_key_bank_selected != new_bank) &&
            (bank_keystate & (1<<g_key_bank_selected))) {
            // NoteOff the old bank.
            midi_stream_note(g_key_bank_selected, false);
        }
        // NoteOn for the new bank every time it's pressed.
        midi_stream_note(new_bank, true);
        g_key_bank_selected = new_bank;
    }

    if (bank_keyup & 0x000f) {
        // NoteOff only for the currently selected bank.
        uint8_t bank_bit = 1 << g_key_bank_selected;
        if (bank_keyup & bank_bit) {
            midi_stream_note(g_key_bank_selected, false);
        }
    }

    // Loop over the key bits and send MIDI messages, converting key
    // numbers to MIDI notes using the mapping table.
    uint16_t bit = 0x0001;
    for(uint8_t i=0; i<keycount; ++i) {
        if (keydown & bit) {
            // There's a key down, put a NoteOn event into the stream.
            uint8_t note = midi_fourbanks_key_to_note(i + keyoffset);
			if (g_device_mode == ABLETON)
			{
				midi_stream_raw_cc(g_midi_channel+1,note,127);			
			}			
            midi_stream_note(note, true);
        }
        if (keyup & bit) {
            // There's a key up, put a NoteOff event onto the stream.
            uint8_t note = midi_fourbanks_key_to_note(i + keyoffset);
            midi_stream_note(note, false);
			if (g_device_mode == ABLETON)
			{
				midi_stream_raw_cc(g_midi_channel+1,note,0);			
			}			
        }
        bit <<= 1;
    }

    if (g_combos_enable) {
		// Recognize combo key events
		// --------------------------
		combo_action_t action = combo_recognize(g_key_down, g_key_up, g_key_state);
		switch (action) {
			case COMBO_A_DOWN:
				midi_stream_note(8, true);
				break;
			case COMBO_A_RELEASE:
				midi_stream_note(8, false);
				break;
			case COMBO_B_DOWN:
				midi_stream_note(9, true);
				break;
			case COMBO_B_RELEASE:
				midi_stream_note(9, false);
				break;
			case COMBO_C_DOWN:
				midi_stream_note(10, true);
				break;
			case COMBO_C_RELEASE:
				midi_stream_note(10, false);
				break;
			case COMBO_D_DOWN:
				midi_stream_note(11, true);
				break;
			case COMBO_D_RELEASE:
				midi_stream_note(11, false);
				break;
			case COMBO_E_DOWN:
				midi_stream_note(12, true);
				break;
			case COMBO_E_RELEASE:
				midi_stream_note(12, false);
				break;
			default:
				// do nothing.
				break;
		}
	}

    // Finished generating MIDI events, flush the endpoints.
    MIDI_Device_Flush(g_midi_interface_info);


    // Update the LEDs ---------------------------------------------------------

    uint16_t leds = 0x0000;

    if (g_key_fourbanks_mode == FOURBANKS_OFF) {

        // Normal display
        // --------------
        // Update the 16 LEDs with the current midi state. Loop over the
        // MIDI keystate array and set an LED bit if that MIDI note has a
        // velocity greater than zero.
        uint8_t basenote = MIDI_BASE_NOTE;
        for (uint8_t i=basenote; i<basenote + 16; ++i) {
            if (g_midi_note_state[i] > 0) {
                leds |= 1 << midi_note_to_key(i);
            }
        }

        // If keypress lights are enabled, illuminate the LED of keys
        // currently activated.
        if (g_led_keypress_enable) {
            leds |= g_key_state;
        }

        // update the external key LEDs.
        uint8_t key_leds = 0;
        uint8_t key_bit = 0x01;
        for (uint8_t i=0; i<4; ++i) {
            if (g_midi_note_state[MIDI_DIGITAL_NOTE + i] > 0) {
                key_leds |= key_bit;
            }
            key_bit <<= 1;
        }
		
		//If exp_keypress leds are enabled, illuminate the LED of keys 
		//currently activated. Warning this bool is hard coded as true
		//unlike g_led_keypress_enable which is set from the EEPROM
		if (g_exp_led_keypress_enable){
			key_leds |= g_exp_key_state;
		}
				
        exp_set_key_led(key_leds);

    } else if (g_key_fourbanks_mode == FOURBANKS_INTERNAL) {

        // Fourbanks Internal
        // ------------------
        //
        // The top four keys display which bank is selected. At least one
        // bank is always selected.
        leds = (1 << g_key_bank_selected);

        // Update the bottom 12 LEDs with the MIDI state of the selected
        // bank.
        uint8_t basenote = MIDI_BASE_NOTE + (g_key_bank_selected * 12);
        for (uint8_t i=basenote; i<basenote + 12; ++i) {
            if (g_midi_note_state[i] > 0) {
                leds |= 1 << midi_fourbanks_note_to_key(i);
            }
        }

        // If keypress lights are enabled, illuminate the LED of the
        // currently activated keys, but only the bottom 12 keys.
        if (g_led_keypress_enable) {
            leds |= g_key_state & 0xfff0;
        }

        // update the external key LEDs.
        uint8_t key_leds = 0;
        uint8_t key_bit = 0x1;
        for (uint8_t i=0; i<4; ++i) {
            if (g_midi_note_state[MIDI_DIGITAL_NOTE + i] > 0) {
                key_leds |= key_bit;
            }
            key_bit <<= 1;
        }
        exp_set_key_led(key_leds);

    } else if (g_key_fourbanks_mode == FOURBANKS_EXTERNAL) {

        // Fourbanks External
        // ------------------
        //
        // Light the external LEDS to indicate the selected bank.
        uint8_t bank_led = 1 << g_key_bank_selected;
        exp_set_key_led(bank_led);

        // set the LED on each key that has a non-zero MIDI state.
        uint8_t basenote = MIDI_BASE_NOTE + (g_key_bank_selected * 16);
        for (uint8_t i=basenote; i<basenote + 16; ++i) {
            if (g_midi_note_state[i] > 0) {
                leds |= 1 << midi_fourbanks_note_to_key(i);
            }
        }

        // If keypress lights are enabled, illuminate the LEDs of the
        // currently activated keys.
        if (g_led_keypress_enable) {
            leds |= g_key_state;
        }

    } // fourbanks mode

    // Illuminate the LEDs with the new pattern.
    led_set_state(leds);

    // Update the Ground Effects LED
    // -----------------------------
    // Use explicit values so we can tweak
    // the flashing pattern: one beat on, three beat off.
    //
    // There are 24 clock ticks per beat, 96 per bar.
    //
    if (g_led_groundfx_counter == 0) {
        led_groundfx_state(true);
    } else if (g_led_groundfx_counter < 8) {
        led_groundfx_state(false);
    } else if (g_led_groundfx_counter < 24) {
        led_groundfx_state(true);
    } else {
        g_led_groundfx_counter = 0;
    }
	
	// Set watchdog flag so main loop knows this section ran
	main_watchdog_flag = true;
	
	DDRD |= 0x02;
	PORTD ^= 0x02;

}

void enter_menu_mode (void)
{
    // Call "key_calc()" to set the "prev_key_state" so that when we
    // enter the menu, the currently held down key will not suddenly
    // count as a keydown and launch a menu item.
    key_calc();
    // Enter the menu system.
    menu();

    // Send reset configuration as sysex
    send_config_data();
}

void factory_reset (void)
{
    // Reset the eeprom values.
    eeprom_factory_reset();

    // Send reset configuration as sysex
    send_config_data();

    // Flash to signal success.
    led_set_state(0xffff);
    _delay_ms(100);
    led_set_state(0x0000);
    _delay_ms(100);
    led_set_state(0xffff);
    _delay_ms(100);
}


// Main -----------------------------------------------------------------------

// Set up ports and peripherals, start the scheduler and never return.
//
int main(void)
{
    // Disable watchdog timer to prevent endless resets if we just used it
    // to soft-reset the machine. (Older AVR chips disable it after reset,
    // the AT90USB162 doesn't).
    MCUSR &= ~(1 << WDRF);  // clear the watchdog reset flag
    wdt_disable();          // turn off the watchdog

    // Disable clock prescaling so we're working at full 16MHz speed.
    // (You'll find this function in winavr/avr/include/avr/power.h)
    clock_prescale_set(clock_div_1);
	

    // Start up the subsystems.
    eeprom_setup();   // setup global settings from the EEPROM
	key_setup();  // startup the key debounce interrupt.
    spi_setup();  // startup the SPI bus.
    led_setup();  // startup the LED chip.
    exp_setup();  // startup the expansion ports and ADC.
    midi_setup(); // startup the MIDI keystate and LUFA MIDI Class interface.
	config_setup();   // setup the configuration system

    // Power-on light show. Woo! This generally signals that we are alive.
    led_count_all_leds();

	// PCB version MF_MK1-3 has an issue where the clock inhibit pins for the 
	// 74HC165 shift registers are floating causing a delay of approximately
	// 1.5 s before buttons can be read correctly after a hard reset. This
	// delay is necessary to mask the problem on this board version. 
	_delay_ms(1500);
	
	// Check to see if the bootloader has been requested by the user holding
    // down the four corner keys at power on time. We have to do this before
    // the USB scheduler starts because shutting down these subsystems
    // before entering the bootloader is a little involved. A little delay is needed
	// so keys are otherwise the debounce will mask the keypress
	
	key_read();
	if (g_key_state == 0x9009) {

		
	    // Drop to Bootloader:
        //  # . . #
        //  . . . .
        //  . . . .
        //  # . . #

        // Signal entering bootloader with a debug check to make sure the
        // BOOTSZ bits are what we think they are (they should be 00). Set the
        // LEDS to reflect the bits and to show that we're entering bootloader.
		 
        //uint8_t fuses = GET_LOW_FUSE_BITS;
        //uint16_t leds= 0xA5A5;
        //if (fuses & FUSE_BOOTSZ0) leds |= 0xff00;
        //if (fuses & FUSE_BOOTSZ1) leds |= 0x00ff;
        //led_set_state(leds);

        // turn off the key debounce interrupt. If we don't do this then as
        // the bootloader is trying to set up it's state the timer interrupt
        // will be firing and jumping to the reset vector 1000 times a
        // second, effectively locking up the machine. There, I just saved
        // you weeks of debugging.
       // key_disable();

        // Assuming the BOOTSZ bits are "00", giving us 4KB of Bootloader
        // space and 12Kb of Program space. The bootloader therefore starts
        // at 0x3000. For more details see the AT90USB162 docs, Table 23-8,
        // page 239.
	    //  asm("ldi r30, 0x00");
	    // asm("ldi r31, 0x30");
	    //  asm("icall");
	    // The assembly code preceding was originally used, but broke when code 
		// size exceeded 8K
	
		//asm("jmp 0x3000");
		
        // We should never reach here. If you see this LED pattern:
        // then something went horribly wrong.
        //  . . . o
        //  . . o .
        //  . o . .
        //  o . . .
       // led_set_state(0x8421);
       // while(1);
		
		led_set_state(0xA5A5);
		Jump_To_Bootloader();

    }  else if(g_key_state == 0x0001) {
        // Menu mode has been requested:
        //  # . . .
        //  . . . .
        //  . . . .
        //  . . . .

        // Call "key_calc()" to set the "prev_key_state" so that when we
        // enter the menu, the currently held down key will not suddenly
        // count as a keydown and launch a menu item.
        key_calc();
        // Enter the menu system.
        menu();
        // when menu exists, we continue the USB startup...

    } else if (g_key_state == 0x1248) {
        // Factory reset all persistent values then drop to menu mode
        //  . . . #
        //  . . # .
        //  . # . .
        //  # . . .

        // Reset the eeprom values.
        eeprom_factory_reset();

        // Flash to signal success.
        led_set_state(0xffff);
		_delay_ms(100);
		led_set_state(0x0000);
        _delay_ms(100);
        led_set_state(0xffff);
        _delay_ms(100);

        // Enter menu mode.
        key_calc();
        menu();
    }

    // Start up USB system now that everything else is safely squared away
    // and our globals are setup.
    USB_Init();

    // enable global interrupts.
    sei();

    // Indicate USB not ready.
    led_set_state(0x0001);

    // Enter an endless loop.
    for(;;) {
        // Read keys and expansion port to check for MIDI events to send and
        // LEDs to set.
        Midifighter_Task();

        // Let the LUFA MIDI Device drivers have a go.
        MIDI_Device_USBTask(g_midi_interface_info);

        // Update the USB state.
        USB_USBTask();
		
		// Reset the watch dog timer, dawg
		if (main_watchdog_flag)
		{
			wdt_reset();
			main_watchdog_flag = false;			
		}
    }
}

// -----------------------------------------------------------------------------
