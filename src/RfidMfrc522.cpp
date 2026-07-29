#include <Arduino.h>
#include "settings.h"

#include "AudioPlayer.h"
#include "HallEffectSensor.h"
#include "Log.h"
#include "MemX.h"
#include "Queues.h"
#include "Rfid.h"
#include "RfidConfig.h"
#include "System.h"

#include <esp_task_wdt.h>

#if defined(RFID_READER_TYPE_RUNTIME)
	#include <MFRC522.h>
	#define MFRC522_firmware_referenceV0_0
	#define MFRC522_firmware_referenceV1_0
	#define MFRC522_firmware_referenceV2_0
	#define FM17522_firmware_reference
	#include "Wire.h"

	#include <MFRC522_I2C.h>

extern unsigned long Rfid_LastRfidCheckTimestamp;
extern TaskHandle_t rfidTaskHandle;
static void RfidMfrc522_Task(void *parameter);

// Cached once at init rather than read from NVS on every task-loop iteration; a restart is required
// for a change to take effect, same as the other MFRC522/PN5180-specific settings.
static uint16_t rfidScanInterval = 100;

	#if defined(RFID_READER_TYPE_RUNTIME)
		#if defined(I2C_2_ENABLE)
extern TwoWire i2cBusTwo;
static MFRC522_I2C mfrc522I2C(MFRC522_ADDR, RST_PIN, &i2cBusTwo);
		#endif
static MFRC522 mfrc522(RFID_CS, RST_PIN);
	#endif

void RfidMfrc522_Init(uint8_t readerType) {
	rfidScanInterval = gPrefsRfid.getUShort("rfidScanIntv", 100);
	uint8_t rfidGain = gPrefsRfid.getUChar("mfrc522Gain", 7u); // default to maximum gain
	rfidGain = (rfidGain & 0x07) << 4; // only lower 3 bits are valid, shift to correct position for register
	if (readerType == RfidReaderType::TYPE_MFRC522_SPI) {
		SPI.begin(RFID_SCK, RFID_MISO, RFID_MOSI, RFID_CS);
		SPI.setFrequency(1000000);
		mfrc522.PCD_Init();
		delay(10);
		// byte firmwareVersion = mfrc522.PCD_ReadRegister(MFRC522::VersionReg);
		// Log_Printf(LOGLEVEL_DEBUG, "RC522 firmware version=%#lx", firmwareVersion);
		mfrc522.PCD_SetAntennaGain(rfidGain);
	} else if (readerType == RfidReaderType::TYPE_MFRC522_I2C) {
	#if defined(I2C_2_ENABLE)
		mfrc522I2C.PCD_Init();
		delay(10);
		// byte firmwareVersion = mfrc522I2C.PCD_ReadRegister(MFRC522_I2C::VersionReg);
		// Log_Printf(LOGLEVEL_DEBUG, "RC522 I2C firmware version=%#lx", firmwareVersion);
		mfrc522I2C.PCD_SetAntennaGain(rfidGain);
	#endif
	} else {
		Log_Println("RfidMfrc522_Init: unsupported reader type", LOGLEVEL_ERROR);
		return;
	}

	delay(50);
	Log_Println(rfidScannerReady, LOGLEVEL_DEBUG);

	if (rfidTaskHandle == NULL) {
		xTaskCreatePinnedToCore(
			RfidMfrc522_Task, /* Function to implement the task */
			"rfid", /* Name of the task */
			3072, /* Stack size in words */
			NULL, /* Task input parameter */
			2 | portPRIVILEGE_BIT, /* Priority of the task */
			&rfidTaskHandle, /* Task handle. */
			0 /* Core where the task should run */
		);
	}
}

void RfidMfrc522_TaskReset(void) {
	Rfid_LastRfidCheckTimestamp = millis();
}

// Deterministic "is a card still on the antenna?" poll used by pauseIfRfidRemoved
// mode. The card is kept parked in the ISO-14443 HALT state between polls; WUPA
// (PICC_WakeupA, 0x52) is the only REQ-family command that wakes a HALTed card,
// so each poll is a clean yes/no. This replaces the old REQA-based detection
// (PICC_IsNewCardPresent sends REQA, 0x26, which only invites cards in the IDLE
// state) whose non-deterministic misses on a perfectly stationary card forced an
// ever-growing miss debounce. Templated because Reader is either the SPI MFRC522
// or the I2C MFRC522_I2C class; the Reader:: register/status constants and the
// PICC_WakeupA return type both differ between the two libraries, so we let the
// compiler pick the right ones per instantiation.
template <typename Reader>
static bool RfidMfrc522_CardStillPresent(Reader &reader, uint8_t *outStatus = nullptr) {
	byte bufferATQA[2];
	byte bufferSize = sizeof(bufferATQA);
	// Reset baud-rate / modulation-width registers exactly like
	// PICC_IsNewCardPresent() does internally; some readers won't answer WUPA
	// reliably otherwise. Reader::* resolves to the correct (SPI-shifted vs I2C)
	// register addresses for whichever library this is instantiated with.
	reader.PCD_WriteRegister(Reader::TxModeReg, 0x00);
	reader.PCD_WriteRegister(Reader::RxModeReg, 0x00);
	reader.PCD_WriteRegister(Reader::ModWidthReg, 0x26);
	auto result = reader.PICC_WakeupA(bufferATQA, &bufferSize);
	// Immediately park the card back in HALT so the next WUPA is meaningful.
	reader.PICC_HaltA();
	if (outStatus != nullptr) {
		*outStatus = static_cast<uint8_t>(result);
	}
	return (result == Reader::STATUS_OK || result == Reader::STATUS_COLLISION);
}

// DEBUG-ONLY (branch debug/wupa-trace): after a WUPA miss, ask again with REQA.
// REQA (0x26) only invites cards in the IDLE state; WUPA (0x52) invites IDLE *and*
// HALT. So the pair discriminates the two candidate faults:
//   REQA answers where WUPA didn't -> the card is present and powered, it just
//                                     ignores WUPA-from-HALT (card-type quirk).
//   both fail                      -> the card isn't being powered/heard at all
//                                     (coupling, field strength, interference).
template <typename Reader>
static uint8_t RfidMfrc522_ProbeReqa(Reader &reader) {
	byte bufferATQA[2];
	byte bufferSize = sizeof(bufferATQA);
	reader.PCD_WriteRegister(Reader::TxModeReg, 0x00);
	reader.PCD_WriteRegister(Reader::RxModeReg, 0x00);
	reader.PCD_WriteRegister(Reader::ModWidthReg, 0x26);
	auto result = reader.PICC_RequestA(bufferATQA, &bufferSize);
	reader.PICC_HaltA();
	return static_cast<uint8_t>(result);
}

template <typename Reader>
static void RfidMfrc522_TaskImpl(Reader &reader) {
	byte lastValidcardId[cardIdSize] = {0}; // must outlive loop iterations: "same card reapplied" is decided by comparing against it

	for (;;) {
		if (rfidScanInterval / 2 >= 20) {
			vTaskDelay(portTICK_PERIOD_MS * (rfidScanInterval / 2));
		} else {
			vTaskDelay(portTICK_PERIOD_MS * 20);
		}
		if (Rfid_ConsumeLastTagReset()) {
			// An assignment changed (or the web UI started playback): the card on/near the reader may now
			// mean something else, so it must not be treated as "same card re-applied" any more.
			memset(lastValidcardId, 0, sizeof(lastValidcardId));
		}

		byte cardId[cardIdSize];
		String cardIdString;
		bool sameCardReapplied = false;
		if ((millis() - Rfid_LastRfidCheckTimestamp) >= rfidScanInterval) {
			// Log_Printf(LOGLEVEL_DEBUG, "%u", uxTaskGetStackHighWaterMark(NULL));

			Rfid_LastRfidCheckTimestamp = millis();
			// Reset the loop if no new card is present on the sensor/reader. This saves the entire process when idle.

			if (!reader.PICC_IsNewCardPresent()) {
				continue;
			}

			// Select one of the cards
			if (!reader.PICC_ReadCardSerial()) {
				continue;
			}

			if (!gPlayProperties.pauseIfRfidRemoved) {
				reader.PICC_HaltA();
				reader.PCD_StopCrypto1();
			}

			memcpy(cardId, reader.uid.uidByte, cardIdSize);

	#ifdef HALLEFFECT_SENSOR_ENABLE
			cardId[cardIdSize - 1] = cardId[cardIdSize - 1] + gHallEffectSensor.waitForState(HallEffectWaitMS);
	#endif

			if (memcmp((const void *) lastValidcardId, (const void *) cardId, sizeof(cardId)) == 0) {
				sameCardReapplied = true;
			}

			String hexString;
			for (uint8_t i = 0u; i < cardIdSize; i++) {
				char str[4];
				snprintf(str, sizeof(str), "%02x%c", cardId[i], (i < cardIdSize - 1u) ? '-' : ' ');
				hexString += str;
			}
			Log_Printf(LOGLEVEL_NOTICE, rfidTagDetected, hexString.c_str());

			for (uint8_t i = 0u; i < cardIdSize; i++) {
				char num[4];
				snprintf(num, sizeof(num), "%03d", cardId[i]);
				cardIdString += num;
			}

			if (gPlayProperties.pauseIfRfidRemoved) {
				if (!sameCardReapplied || gPlayProperties.trackFinished || gPlayProperties.playlistFinished) { // Don't allow to send card to queue if it's the same card again if track or playlist is unfnished
					xQueueSend(gRfidCardQueue, cardIdString.c_str(), 0);
				} else {
					// If pause-button was pressed while card was not applied, playback could be active. If so: don't pause when card is reapplied again as the desired functionality would be reversed in this case.
					if (gPlayProperties.pausePlay && System_GetOperationMode() != OPMODE_BLUETOOTH_SINK) {
						AudioPlayer_SetTrackControl(PAUSEPLAY); // ... play/pause instead (but not for BT)
					}
				}
				memcpy(lastValidcardId, reader.uid.uidByte, cardIdSize);
			} else {
				xQueueSend(gRfidCardQueue, cardIdString.c_str(), 0); // If pauseIfRfidRemoved isn't active, every card-apply leads to new playlist-generation
			}

			if (gPlayProperties.pauseIfRfidRemoved) {
				// Park the freshly-selected card in the HALT state so the WUPA-based
				// presence poll below can wake it deterministically. Without this the
				// card is left ACTIVE and only REQA (which ignores ACTIVE/HALT cards)
				// was available, causing the notorious pause/resume flap on stationary
				// cards. See RfidMfrc522_CardStillPresent().
				reader.PICC_HaltA();
				reader.PCD_StopCrypto1();

				// Poll until the card is physically removed. Each WUPA poll is a clean
				// yes/no, so a small debounce is enough to swallow the rare genuinely
				// dropped poll (RF noise) without the old REQA "voodoo". Set to 1 to
				// test raw WUPA reliability with zero tolerance for a missed poll.
				constexpr uint8_t removalDebounceCycles = 2;
				// Backstop for the "errors instead of timeouts" case; long enough that a noise
				// burst over a resting card never trips it, short enough that a real removal
				// masked by noise is still noticed quickly.
				constexpr uint32_t noAnswerTimeoutMs = 1500;
				uint8_t consecutiveMisses = 0;
				uint32_t pollCount = 0;
				uint32_t okCount = 0;
				uint32_t noiseCount = 0;
				const uint32_t pollStart = millis();
				uint32_t lastAnswerAt = millis();
				while (true) {
					if (rfidScanInterval / 2 >= 20) {
						vTaskDelay(portTICK_PERIOD_MS * (rfidScanInterval / 2));
					} else {
						vTaskDelay(portTICK_PERIOD_MS * 20);
					}
					uint8_t wupaStatus = 0xEE;
					pollCount++;
					if (RfidMfrc522_CardStillPresent(reader, &wupaStatus)) {
						okCount++;
						if (consecutiveMisses > 0) {
							Log_Printf(LOGLEVEL_NOTICE, "WUPA-DBG: recovered after %u miss(es) [poll=%u ok=%u]", consecutiveMisses, pollCount, okCount);
						}
						consecutiveMisses = 0;
						lastAnswerAt = millis();
					} else if (wupaStatus != static_cast<uint8_t>(Reader::STATUS_TIMEOUT)) {
						// Something answered and the frame was mangled (parity/protocol/CRC error
						// out of the MFRC522's ErrorReg) -- that is evidence the card is STILL
						// THERE, not that it left: an empty field produces a clean timeout, since
						// there is nobody to reply at all. Counting these as misses is what makes
						// a card in an electrically noisy spot pause every few seconds. Ignore
						// them, but keep the staleness watchdog below honest.
						noiseCount++;
						Log_Printf(LOGLEVEL_NOTICE, "WUPA-DBG: noise (status=%u) ignored [poll=%u ok=%u noise=%u]", wupaStatus, pollCount, okCount, noiseCount);
					} else if (++consecutiveMisses >= removalDebounceCycles) {
						break;
					}

					// Watchdog: if nothing has answered cleanly for a while -- e.g. the card was
					// lifted during a noise burst, so we keep seeing errors instead of timeouts --
					// declare removal anyway rather than polling a card that is long gone.
					if ((millis() - lastAnswerAt) >= noAnswerTimeoutMs) {
						Log_Printf(LOGLEVEL_NOTICE, "WUPA-DBG: no clean answer for %ums -- treating as removed", millis() - lastAnswerAt);
						break;
					}
				}
				Log_Printf(LOGLEVEL_NOTICE, "WUPA-DBG: removal declared -- %u polls, %u ok, %u noise, held %ums", pollCount, okCount, noiseCount, millis() - pollStart);

				Log_Println(rfidTagRemoved, LOGLEVEL_NOTICE);
				// Only pause if there's actually something to pause -- otherwise removing a card after the
				// playlist has already finished naturally queues a PAUSEPLAY that AudioPlayer_Cyclic() then
				// rejects with "no playmode change while idle", which is a confusing error for a normal action.
				if (!gPlayProperties.pausePlay && !gPlayProperties.playlistFinished && gPlayProperties.playMode != NO_PLAYLIST && System_GetOperationMode() != OPMODE_BLUETOOTH_SINK) {
					AudioPlayer_SetTrackControl(PAUSEPLAY);
					Log_Println(rfidTagReapplied, LOGLEVEL_NOTICE);
				}
				reader.PICC_HaltA();
				reader.PCD_StopCrypto1();

				// A card is only re-detected by the loop above via PICC_IsNewCardPresent(),
				// which sends REQA (0x26) -- and REQA is ignored by cards in the HALT state.
				// Every card we have seen is parked in HALT by the presence poll, so if the
				// removal was spurious (one noisy poll, card never actually left) the card
				// sits halted in a live field and REQA can never see it again: playback
				// stays paused until the user physically lifts and re-applies the card.
				// Drop the field briefly so any card still on the antenna loses power and
				// powers back up in IDLE, where REQA finds it.
				reader.PCD_AntennaOff();
				vTaskDelay(portTICK_PERIOD_MS * 10);
				reader.PCD_AntennaOn();
			}
		}
	}
}

void RfidMfrc522_Task(void *parameter) {
	if (RfidConfig_GetReaderType() == RfidReaderType::TYPE_MFRC522_I2C) {
	#if defined(I2C_2_ENABLE)
		RfidMfrc522_TaskImpl(mfrc522I2C);
	#endif
	} else {
		RfidMfrc522_TaskImpl(mfrc522);
	}
}

void RfidMfrc522_Cyclic(void) {
	// Not necessary as cyclic stuff performed by task Rfid_Task()
}

void RfidMfrc522_Exit(void) {
	Log_Println("shutdown MFRC522..", LOGLEVEL_NOTICE);
	if (RfidConfig_GetReaderType() != RfidReaderType::TYPE_MFRC522_I2C) {
		mfrc522.PCD_SoftPowerDown();
	}
	if (rfidTaskHandle != NULL) {
		vTaskDelete(rfidTaskHandle);
		rfidTaskHandle = NULL;
	}
}

void RfidMfrc522_WakeupCheck(void) {
}

#endif
