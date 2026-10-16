#pragma once

#include <array>
#include <queue>
#include "../Iop_Module.h"
#include "../Iop_SifMan.h"
#include "../../SifModuleAdapter.h"
#include "../../PadInterface.h"
#include "../../ScreenPositionListener.h"
#include "filesystem_def.h"
#ifdef _WIN32
#include "iop/namco_sys246/MameCompatOutput.h"
#endif

namespace Iop
{
	class CSifCmd;

	namespace Namco
	{
		class CAcRam;

		class CSys246 : public CModule, public CPadInterface, public CScreenPositionListener
		{
		public:
			enum class JVS_MODE
			{
				DEFAULT,
				LIGHTGUN,
				DRUM,
				DRIVE,
				TOUCH,
			};

			CSys246(CSifMan&, CSifCmd&, Namco::CAcRam&, const std::string&);
			virtual ~CSys246() = default;

			std::string GetId() const override;
			std::string GetFunctionName(unsigned int) const override;
			void Invoke(CMIPS&, unsigned int) override;

			void SaveState(Framework::CZipArchiveWriter&) const override;
			void LoadState(Framework::CZipArchiveReader&) override;

			void SetJvsMode(JVS_MODE);
			void SetBoardId(std::string);
			//Drives the shutter emulation, see the machine definition setting of the same name.
			void SetCardReaderShutter(bool value) { m_cardReaderShutter = value; }
			void SetButton(unsigned int, PS2::CControllerInfo::BUTTON);
			void SetScreenPosXform(const std::array<float, 4>&);

			//CPadInterface
			void SetButtonState(unsigned int, PS2::CControllerInfo::BUTTON, bool, uint8*) override;
			void SetAxisState(unsigned int, PS2::CControllerInfo::BUTTON, uint8, uint8*) override;
			void GetVibration(unsigned int, uint8& largeMotor, uint8& smallMotor) override{};

			//CScreenPositionListener
			void SetScreenPosition(float, float) override;
			void ReleaseScreenPosition() override;

		private:
			enum MODULE_ID
			{
				MODULE_ID_1 = 0x76500001,
				MODULE_ID_3 = 0x76500003,
				MODULE_ID_4 = 0x76500004,
				MODULE_ID_SERIAL = 0x00830120,
			};

			enum COMMAND_ID
			{
				COMMAND_ID_ACFLASH = 3,
			};

			enum
			{
				SERIAL_SETUP = 1,
				SERIAL_2 = 2,
				SERIAL_READ = 3,
				SERIAL_WRITE = 4,
				SERIAL_5 = 5,
				SERIAL_WRITE_READ = 10,
				SERIAL_12 = 12,
				SERIAL_13 = 13,
				SERIAL_14 = 14,
				SERIAL_15 = 15,
			};

			enum
			{
				BACKUP_RAM_SIZE = 0x10000,
			};

			enum
			{
				JVS_PLAYER_COUNT = 2,
			};

			//Card reader shutter door. The game drives its motor through a JVS digital output
			//and reads its position back from the switch inputs. While the motor is powered,
			//the door moves from one position to the next one:
			//closed -> middle -> open -> middle -> closed -> ...
			enum
			{
				DOOR_STATE_CLOSED = 0,
				DOOR_STATE_MIDDLE_A = 1,
				DOOR_STATE_OPEN = 2,
				DOOR_STATE_MIDDLE_B = 3,
				DOOR_STATE_COUNT = 4,

				//Number of switch polls needed to move the door by one position
				DOOR_MOTOR_STEP_POLLS = 60,

				//Bit of the first JVS digital output that powers the shutter motor. The games drive
				//0x60 for the pair of cabinet lamps (which stay on) and 0x70 while the door moves,
				//so the motor is bit 4 - the fifth of the eight output LEDs on the real I/O board.
				DOOR_MOTOR_OUTPUT_MASK = 0x0010,

				//Switch bits the door position is reported on. These are the bits the default
				//JVS button mapping puts DPAD_LEFT, CIRCLE and DPAD_RIGHT on.
				DOOR_SWITCH_BITS_MASK = 0x000D,
				DOOR_SWITCH_BITS_CLOSED = 0x0008,
				DOOR_SWITCH_BITS_MIDDLE = 0x0001,
				DOOR_SWITCH_BITS_OPEN = 0x0004,
			};

			enum JVS_DRUM_CHANNELS
			{
				JVS_DRUM_CHANNEL_1P_DL,
				JVS_DRUM_CHANNEL_2P_KL,
				JVS_DRUM_CHANNEL_2P_DL,
				JVS_DRUM_CHANNEL_1P_DR,
				JVS_DRUM_CHANNEL_1P_KR,
				JVS_DRUM_CHANNEL_1P_KL,
				JVS_DRUM_CHANNEL_2P_KR,
				JVS_DRUM_CHANNEL_2P_DR,
				JVS_DRUM_CHANNEL_MAX,
			};

			enum JVS_WHEEL_CHANNELS
			{
				JVS_WHEEL_CHANNEL_WHEEL,
				JVS_WHEEL_CHANNEL_GAZ,
				JVS_WHEEL_CHANNEL_BRAKE,
				JVS_WHEEL_CHANNEL_MAX,
			};

			bool Invoke001(uint32, uint32*, uint32, uint32*, uint32, uint8*);
			bool Invoke003(uint32, uint32*, uint32, uint32*, uint32, uint8*);
			bool Invoke004(uint32, uint32*, uint32, uint32*, uint32, uint8*);
			bool InvokeSerial(uint32, uint32*, uint32, uint32*, uint32, uint8*);

			void ProcessAcFlashCommand(const SIFCMDHEADER*, CSifMan&);

			void ProcessJvsPacket(const uint8*, uint8*);
			void ProcessBgStrPacket(const uint8*, uint8*, int);

			void SetDoorMotorState(bool);
			void TickDoor();
			uint16 GetDoorSwitchBits() const;

			static fs::path GetArcadeSavePath();
			void ProcessMemRequest(uint8*, uint32);
			void ReadBackupRam(uint32, uint8*, uint32);
			void WriteBackupRam(uint32, const uint8*, uint32);

			Namco::CAcRam& m_acRam;

			CSifModuleAdapter m_module001;
			CSifModuleAdapter m_module003;
			CSifModuleAdapter m_module004;
			CSifModuleAdapter m_moduleSerial;

			std::string m_gameId;
			uint32 m_recvAddr = 0;
			uint32 m_sendAddr = 0;

			JVS_MODE m_jvsMode = JVS_MODE::DEFAULT;
			std::array<float, 4> m_screenPosXform = {65535, 0, 65535, 0};
			std::string m_boardId = "namco ltd.;TSS-I/O;Ver2.11;GUN-EXTENTION";

			std::array<uint16, PS2::CControllerInfo::MAX_BUTTONS> m_jvsButtonBits = {};
			uint16 m_jvsButtonState[JVS_PLAYER_COUNT] = {};
			uint16 m_jvsSystemButtonState = 0;
			uint16 m_jvsScreenPosX = 0x7FFF;
			uint16 m_jvsScreenPosY = 0x7FFF;
			uint16 m_jvsDrumChannels[JVS_DRUM_CHANNEL_MAX] = {};
			uint16 m_jvsWheelChannels[JVS_WHEEL_CHANNEL_MAX] = {};
			uint16 m_jvsWheel = 0x0;
			uint16 m_jvsGaz = 0x0;
			uint16 m_jvsBrake = 0x0;
			uint16 m_coin1 = 0;
			uint16 m_coin2 = 0;
			uint8 m_testButtonState = 0;
			uint8 m_counter = 0;

			std::queue<uint8> m_serialQueue;
			bool m_bgStrReportWheelPos = false;

#ifdef _WIN32
			// MAME-compatible network output listener
			std::unique_ptr<MameCompatOutput> m_mameCompatOutput;
#endif

			// track last recoil state to limit events
			uint8 m_p1RecoilLast = 0;

			//Card reader door emulation
			uint32 m_doorState = DOOR_STATE_CLOSED;
			//When clear the shutter is not driven at all and reports as open, so games that
			//wait for the door to move keep going.
			bool m_cardReaderShutter = true;
			uint32 m_doorPollCounter = 0;
			bool m_doorMotorOn = false;

			//Diagnostics for the arcade I/O command traffic. The last packets seen are kept
			//as raw bytes so that change detection stays cheap on the polling path.
			std::string m_lastJvsRequestRaw;
			uint32 m_jvsRequestDumpCount = 0;
			std::string m_lastJvioSendRaw;
			uint32 m_jvioSendDumpCount = 0;
			uint32 m_pktIdLogCount = 0;
			uint32 m_jvsChecksumErrorCount = 0;
			uint32 m_unknownJvsCommandCount = 0;
		};
	}
}
