#pragma once

#include "Types.h"
#include "../Iop_Module.h"
#include "filesystem_def.h"
#include <map>
#include <string>
#include <vector>

namespace Iop
{
	namespace Namco
	{
		//Logs calls made to arcade IOP modules that are provided by the SYS2x6 BIOS but are
		//not implemented by the emulator (acdev, acuart, acjv, ...).
		//Without this, import calls made by the games are silently dropped, which makes the
		//games believe the arcade hardware (such as the card reader connected to the arcade
		//serial port) is absent.
		//For acuart, the card reader connected to the port is modelled in code.
		class CArcadeSpy : public CModule
		{
		public:
			//With the internal reader disabled the card commands are only forwarded to the
			//serial port, leaving the cards to the real hardware or an external manager.
			void SetInternalCardReader(bool value) { m_internalCardReader = value; }
			//Selects the serial port of the real card reader, see the "arcadeSerialPort" setting.
			//The port is only used when the reader is not emulated internally, and it is opened
			//here because the value only becomes known after the module was constructed. Opening
			//is idempotent, only the first instance that gets here wins.
			void SetSerialPort(const std::string& value)
			{
				m_serialPort = value;
				if(!m_internalCardReader)
				{
					OpenSerialPortIfConfigured();
				}
			}
			CArcadeSpy(std::string, uint8*);

			std::string GetId() const override;
			std::string GetFunctionName(unsigned int) const override;
			void Invoke(CMIPS&, unsigned int) override;

		private:
			//A request sent by the game and everything the card reader answers to it
			struct UART_COMMAND_ENTRY
			{
				std::vector<uint8> request;
				std::vector<std::vector<uint8>> replies;
				bool hasAck = false;
			};

			void QueueUartCommand(const uint8* command, uint32 size);
			void QueueUartReply();
			bool ShouldLogUartCommand();
			uint32 ServeUartRead(uint32 bufferPtr, uint32 size);

			//Card emulation
			void ScanCards();
			void LoadCard(size_t);
			void SaveCard();
			void HandleCardRequest(const uint8*, uint32);
			void HandleCardReply(std::vector<uint8>&);
			std::vector<uint8> BuildSyntheticCardReply(uint8);
			std::vector<uint8> GetCardStatusReply(uint8);
			void TickCardState();
			void UpdateCardCommandState(uint8);
			void UpdateCardControl();
			void ApplyCardControlCommand(const std::string&);

			std::string m_moduleId;
			uint8* m_iopRam = nullptr;
			std::map<unsigned int, uint32> m_callCounts;

			//Arcade interface timing registers (acdev: Set/GetAcMemDelayReg, Set/GetAcIoDelayReg)
			uint32 m_memDelayReg = 0x1A30FF;
			uint32 m_ioDelayReg = 0x1A30FF;

			//Acuart port attributes (acuart: acUartSetAttr)
			uint32 m_uartSpeed = 9600;

			//Card reader emulation state
			std::vector<uint8> m_uartReadQueue;
			uint32 m_uartCommandLogCount = 0;

			//Cards are stored as images (<Play! data path>/arcadecards/cardN.pbm) holding
			//the printed artwork of the card, with the card data hidden behind a magic
			//appended at the end of the file.
			fs::path m_cardPath;
			size_t m_cardCount = 0;
			size_t m_cardIndex = 0;
			//Card number a card that is dispensed by the game should be stored as, set by
			//take=N. It is only used when the card in the reader did not come from the store, and
			//it falls back to the highest number plus one when that card already exists.
			static constexpr size_t CARD_NO_SAVE_TARGET = static_cast<size_t>(-1);
			size_t m_cardSaveAsIndex = CARD_NO_SAVE_TARGET;
			//Set while the card in the reader came from the store (insert=N or the advance after
			//a read). A card the game dispensed itself leaves it clear, and that is what decides
			//whether the card keeps its own file or gets a new number.
			bool m_cardFromStore = false;
			bool m_cardsScanned = false;
			std::map<uint32, std::vector<uint8>> m_cardBlocks;
			std::vector<uint8> m_cardImage;
			std::vector<uint8> m_cardTextFrames;
			//Response bytes of the reader: card position, error code and command state
			//('0' none, '1'/'3' card in the device, '4' card at the insertion slot)
			uint8 m_cardState1 = '0';
			//('0' finished, '2' cannot execute, '3' in progress, '4' waiting for a card,
			// '5' no card in the dispenser)
			uint8 m_cardState3 = '0';
			uint32 m_cardBusyPolls = 0;
			uint32 m_cardSlotPolls = 0;
			//A card is waiting at the insertion slot to be taken in by the reader. Response 1
			//never reports '4' for it: in the SDK '4' means a card that was left behind at the
			//slot, so the card is reported as inside the reader ('1') as soon as a command
			//takes it in, with response 3 going from '3' (executing) to '0' (finished).
			bool m_cardPendingInsert = false;
			//Set when the game prints on the card of the reader: the artwork that is saved with
			//the card is then the printed one, otherwise the card keeps the picture it had
			bool m_cardImagePrinted = false;
			//Blank cards available in the dispenser of the cabinet
			uint32 m_cardDispenserCards = 20;
			//Serial port of the real card reader, taken from the machine definition
			//("arcadeSerialPort", for instance "COM6"). Empty means no port is opened and
			//the reader is emulated as usual.
			std::string m_serialPort;
			//Shared by every instance of this class: the module that provides the arcade
			//serial port is not necessarily the instance that opened the port
			static bool m_serialPassthrough;
			static void* m_serialHandle;
			void OpenSerialPortIfConfigured();
			uint32 ReadSerialPort(uint8*, uint32);
			void WriteSerialPort(const uint8*, uint32);
			//Cards are inserted and ejected by hand through <Play! data path>/arcade_card.txt
			//("feed", "insert=N", "eject", "take", "dispenser=N", "slot=N", "manual")
			fs::path m_cardControlPath;
			//When clear, the reader only forwards what it receives and lets the real hardware (or
			//an external manager on the serial port) handle the card protocol.
			bool m_internalCardReader = true;

			fs::file_time_type m_cardControlTime;
			bool m_cardControlLoaded = false;
			uint32 m_cardControlCheckCounter = 0;
			//When automatic, the emulator feeds a card and takes it away by itself, otherwise
			//every move of the card is made through the control file
			//Cards go in and out by hand: there is no automatic mode any more.
			bool m_cardAuto = false;
			//Number of polls an ejected card stays at the slot before being taken away
			//automatically (0 disables it)
			uint32 m_cardSlotTakePolls = 20;
			//Every command code the game has used so far, so that new ones can be reported
			bool m_seenCardCommands[256] = {};
			std::vector<uint8> m_syntheticReply;
			bool m_useSyntheticReply = false;
			uint32 m_cardCleanStep = 0;
			uint32 m_cardLoadStep = 0;
			//State of the command the game is waiting on. A real reader acknowledges every
			//command frame with 0x06 and the game asks for the result with 0x05, which is
			//answered with the state of the command at that moment: the reply of a read that
			//is waiting for a card changes by itself once the card is inserted.
			struct CARD_COMMAND_STATE
			{
				uint8 command = 0;
				uint8 parameter[3] = {'0', '0', '0'};
				uint32 enqCount = 0;
				bool active = false;
			};
			CARD_COMMAND_STATE m_cardCommand;
			bool HandleModelledCardCommand(const uint8*, uint32);
			std::vector<uint8> BuildCardCommandReply();
		};
	}
}
