#include "Iop_NamcoArcadeSpy.h"
#include <cstring>
#include <cstdarg>
#include <cstdio>
#include <chrono>
#include <fstream>
#ifdef _WIN32
#include <windows.h>
#endif
#include "Ps2Const.h"
#include "AppConfig.h"
#include "Log.h"
#include "PathUtils.h"
#include "StdStreamUtils.h"
#include "string_format.h"

using namespace Iop;
using namespace Iop::Namco;

#define LOG_NAME ("iop_namco_arcadespy")
#define UART_REPLY_FILE ("arcade_uart_reply.txt")

namespace
{
	//Card reader entries are written straight to <Play! data path>/arcade_card_log.txt
	//instead of going through CLog: this keeps them available even when the emulator is
	//built with LOGGING_ENABLED 0, which compiles every CLog call out for speed.
	void CardLog(const char* format, ...)
	{
		static std::ofstream logStream;
		if(!logStream.is_open())
		{
			auto logPath = CAppConfig::GetInstance().GetBasePath() / "arcade_card_log.txt";
			//Binary mode: the entries already contain their own line endings and text mode
			//would translate them again, which adds a blank line after every entry. Append
			//mode because the shutter writes into the same file from another unit.
			logStream.open(logPath.native().c_str(), std::ios::out | std::ios::app | std::ios::binary);
			if(!logStream.is_open()) return;
		}
		va_list args;
		va_start(args, format);
		char buffer[1024];
		vsnprintf(buffer, sizeof(buffer), format, args);
		va_end(args);

		//Every entry is stamped with the time since the process started: this tells a delay of
		//the emulator apart from a delay of the game, which polls the reader at its own pace.
		//Without it, a control that only reaches the game on its next poll looks like a bug of
		//the emulation.
		{
			auto now = std::chrono::steady_clock::now();
			auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
			char timestamp[32];
			snprintf(timestamp, sizeof(timestamp), "[%d.%03d] ",
			         static_cast<int>(elapsed / 1000), static_cast<int>(elapsed % 1000));
			logStream << timestamp;
		}
		logStream << buffer;
		logStream.flush();
	}

	//Calls are logged in full for the first few occurrences of each function, then only
	//once in a while: drivers tend to poll some functions thousands of times in a row.
	static const uint32 FIRST_LOG_COUNT = 8;
	//Per call logging has to be very sparse: the games poll the arcade I/O modules hundreds
	//of times per second and a log entry per call makes them stutter.
	static const uint32 LOG_INTERVAL = 16384;
	static const uint32 MAX_DUMP_SIZE = 0x1000;
	static const uint32 MAX_READ_QUEUE_SIZE = 0x2000;

	static const uint8 UART_ENQ = 0x05;
	static const uint8 UART_ACK = 0x06;

	struct FUNCTION_NAME
	{
		const char* moduleId;
		unsigned int functionId;
		const char* name;
	};

	//Names are taken from the PS2SDK reimplementations of the SYS2x6 BIOS modules.
	//Ref: https://github.com/ps2dev/ps2sdk/tree/master/iop/arcade
	//Note: PS2SDK's acuart export table lists four _retonly entries while every other
	//arcade module (acdev, acata, accore, acjv, ...) has three. Behaviour observed in
	//games (index 8 is called with a (buffer, size) pair while waiting for card reader
	//data) matches the three entry layout, which is what is used here.
	static const FUNCTION_NAME g_functionNames[] =
	{
		{"acdev", 0, "_start"},
		{"acdev", 1, "_retonly"},
		{"acdev", 2, "_retonly"},
		{"acdev", 3, "_retonly"},
		{"acdev", 4, "SetAcMemDelayReg"},
		{"acdev", 5, "GetAcMemDelayReg"},
		{"acdev", 6, "SetAcIoDelayReg"},
		{"acdev", 7, "GetAcIoDelayReg"},

		{"acuart", 0, "_start"},
		{"acuart", 1, "_retonly"},
		{"acuart", 2, "_retonly"},
		{"acuart", 3, "_retonly"},
		{"acuart", 4, "acUartModuleRestart"},
		{"acuart", 5, "acUartModuleStart"},
		{"acuart", 6, "acUartModuleStatus"},
		{"acuart", 7, "acUartModuleStop"},
		{"acuart", 8, "acUartRead"},
		{"acuart", 9, "acUartWrite"},
		{"acuart", 10, "acUartWait"},
		{"acuart", 11, "acUartGetAttr"},
		{"acuart", 12, "acUartSetAttr"},

		{"accore", 0, "_start"},
		{"accore", 4, "acDmaModuleRestart"},
		{"accore", 5, "acDmaModuleStart"},
		{"accore", 6, "acDmaModuleStatus"},
		{"accore", 7, "acDmaModuleStop"},
		{"accore", 8, "acDmaSetup"},
		{"accore", 9, "acDmaRequest"},
		{"accore", 10, "acDmaRequestI"},
		{"accore", 11, "acDmaCancel"},
		{"accore", 12, "acDmaCancelI"},
		{"accore", 13, "acIntrModuleRestart"},
		{"accore", 14, "acIntrModuleStart"},
		{"accore", 15, "acIntrModuleStatus"},
		{"accore", 16, "acIntrModuleStop"},
		{"accore", 17, "acIntrRegister"},
		{"accore", 18, "acIntrRelease"},
		{"accore", 19, "acIntrEnable"},
		{"accore", 20, "acIntrDisable"},
		{"accore", 21, "acIntrClear"},

		{"acjv", 0, "_start"},
		{"acjv", 4, "acJvModuleRestart"},
		{"acjv", 5, "acJvModuleStart"},
		{"acjv", 6, "acJvModuleStatus"},
		{"acjv", 7, "acJvModuleStop"},
		{"acjv", 8, "acJvRead"},
		{"acjv", 9, "acJvWrite"},
		{"acjv", 10, "acJvGet"},
		{"acjv", 11, "acJvPut"},

		{"acata", 0, "_start"},
		{"acata", 4, "acAtaModuleRestart"},
		{"acata", 5, "acAtaModuleStart"},
		{"acata", 6, "acAtaModuleStatus"},
		{"acata", 7, "acAtaModuleStop"},
		{"acata", 8, "acAtaSetup"},
		{"acata", 9, "acAtaRequest"},
		{"acata", 10, "acAtaRequestI"},
		{"acata", 11, "acAtaReply"},
		{"acata", 12, "acAtaStatus"},
		{"acata", 13, "acAtapiSetup"},
		{"acata", 14, "acAtapiRequest"},
		{"acata", 15, "acAtapiRequestI"},
		{"acata", 16, "acAtapiStatus"},
		{"acata", 17, "ata_probe"},

		{"ssbusc", 4, "SetDelay"},
		{"ssbusc", 5, "GetDelay"},
		{"ssbusc", 6, "SetBaseAddress"},
		{"ssbusc", 7, "GetBaseAddress"},
		{"ssbusc", 8, "SetRecoveryTime"},
		{"ssbusc", 9, "GetRecoveryTime"},
		{"ssbusc", 10, "SetHoldTime"},
		{"ssbusc", 11, "GetHoldTime"},
		{"ssbusc", 12, "SetFloatTime"},
		{"ssbusc", 13, "GetFloatTime"},
		{"ssbusc", 14, "SetStrobeTime"},
		{"ssbusc", 15, "GetStrobeTime"},
		{"ssbusc", 16, "SetCommonDelay"},
		{"ssbusc", 17, "GetCommonDelay"},
	};

	const char* GetFunctionNameFor(const std::string& moduleId, unsigned int functionId)
	{
		for(const auto& functionName : g_functionNames)
		{
			if((moduleId == functionName.moduleId) && (functionId == functionName.functionId))
			{
				return functionName.name;
			}
		}
		return nullptr;
	}

	bool IsValidRamRange(uint32 address, uint32 size)
	{
		return (address < PS2::IOP_RAM_SIZE) && (size <= PS2::IOP_RAM_SIZE) && ((address + size) <= PS2::IOP_RAM_SIZE);
	}

	std::string FormatHexDump(const uint8* data, uint32 size, uint32 maxBytes = 0x100)
	{
		uint32 dumpSize = (size > maxBytes) ? maxBytes : size;
		std::string result;
		for(uint32 i = 0; i < dumpSize; i++)
		{
			if(i != 0) result += ((i % 16) == 0) ? "\r\n      " : " ";
			result += string_format("%02X", static_cast<uint32>(data[i]));
		}
		if(dumpSize < size) result += string_format(" ... (%d more bytes)", static_cast<int>(size - dumpSize));
		if(result.empty()) result = "(no data)";
		return result;
	}

	//Parses a byte string written in hexadecimal. Lines of a serial capture log are
	//understood as well.
	std::vector<uint8> ParseHexBytes(const std::string& line)
	{
		std::vector<uint8> result;
		if(line.empty() || (line[0] == '#')) return result;

		std::string hexPart = line;
		auto colonPos = line.find(':');
		if(colonPos != std::string::npos)
		{
			hexPart = line.substr(colonPos + 1);
		}

		int highNibble = -1;
		for(char character : hexPart)
		{
			int value = -1;
			if((character >= '0') && (character <= '9')) value = character - '0';
			else if((character >= 'a') && (character <= 'f')) value = character - 'a' + 10;
			else if((character >= 'A') && (character <= 'F')) value = character - 'A' + 10;
			if(value < 0) continue;
			if(highNibble < 0)
			{
				highNibble = value;
				continue;
			}
			result.push_back(static_cast<uint8>((highNibble << 4) | value));
			highNibble = -1;
		}
		return result;
	}

	//Card reader commands
	static const uint8 CARD_CMD_WRITE = 0x53;
	static const uint8 CARD_CMD_WRITE_TEXT = 0x7C;
	static const uint8 CARD_CMD_WRITE_IMAGE = 0x7B;
	static const uint8 CARD_CMD_EJECT = 0x80;

	//Printed card artwork: bands of 102 rows of 40 bytes (320 pixels, 312 visible),
	//sent as 17 blocks of 240 bytes per band
	//The printed picture is 1 bit per pixel and 312 pixels wide, so a line is 39 bytes with no
	//padding at all: the next line starts right after it. The lines are handed over in blocks
	//of 24 lines (936 bytes), and a 0x7B frame carries one chunk of such a block.
	static const uint32 CARD_IMAGE_ROW_SIZE = 39;
	static const uint32 CARD_IMAGE_GROUP_LINES = 24;
	static const uint32 CARD_IMAGE_GROUP_SIZE = CARD_IMAGE_ROW_SIZE * CARD_IMAGE_GROUP_LINES;
	static const uint32 CARD_IMAGE_CHUNK_SIZE = 241;
	static const uint32 CARD_IMAGE_BLOCK_SIZE = CARD_IMAGE_GROUP_SIZE;
	static const uint32 CARD_IMAGE_BLOCK_COUNT = 21;
	//A card that was never printed gets a blank picture of the first 0x18 (24) blocks, which is
	//the artwork the printed data is written over when the game prints something on it
	static const uint32 CARD_IMAGE_BLANK_GROUPS = 0x18;
	static const uint32 CARD_IMAGE_BLANK_SIZE = CARD_IMAGE_GROUP_SIZE * CARD_IMAGE_BLANK_GROUPS;

	//Number of reader polls before a card waiting at the insertion slot is considered taken
	//away, and number of polls a command reports itself as being executed
	static const uint32 CARD_TAKE_AWAY_POLLS = 20;
	static const uint32 CARD_BUSY_POLLS = 4;

	//Number of commands between two checks of the card control file
	static const uint32 CARD_CONTROL_CHECK_COMMANDS = 32;

	//Parses a decimal number used by the card control file
	bool ParseControlNumber(const std::string& text, uint32& value)
	{
		if(text.empty()) return false;

		uint32 result = 0;
		for(char character : text)
		{
			if((character < '0') || (character > '9')) return false;
			result = (result * 10) + (character - '0');
		}
		value = result;
		return true;
	}

	static const char CARD_FILE_MAGIC[8] = {'I', 'M', 'S', 'C', 'A', 'R', 'D', '1'};

	//Builds a code reply frame: 02 06 CMD <3 digits> 03 CHK
	std::vector<uint8> BuildCardCodeReply(uint8 command, const char* code)
	{
		std::vector<uint8> frame;
		frame.push_back(0x02);
		frame.push_back(0x06);
		frame.push_back(command);
		for(uint32 i = 0; i < 3; i++) frame.push_back(static_cast<uint8>(code[i]));
		frame.push_back(0x03);
		uint8 checksum = 0;
		for(size_t i = 1; i < frame.size(); i++) checksum ^= frame[i];
		frame.push_back(checksum);
		return frame;
	}

	//Names of the card reader commands, used by the log
	const char* GetCardCommandName(uint8 command)
	{
		switch(command)
		{
		case 0x10: return "initialise";
		case 0x20: return "card state";
		case 0x33: return "read data";
		case 0x35: return "read data request";
		case 0x40: return "cancel";
		case 0x53: return "write data";
		case 0x55: return "write and discharge";
		case 0x75: return "print and discharge";
		case 0x78: return "print setup";
		case 0x7A: return "register font";
		case 0x7B: return "print image";
		case 0x7C: return "print text";
		case 0x7D: return "unknown (7D)";
		case 0x80: return "discharge";
		case 0x90: return "store in dispenser";
		case 0xA0: return "cleaning";
		case 0xB0: return "take in from dispenser";
		default: return "unknown";
		}
	}

	//Describes the three response bytes of a reply
	std::string DescribeCardResponse(const uint8* response)
	{
		std::string result;
		switch(response[0])
		{
		case '0': result += "no card"; break;
		case '1': result += "card inside"; break;
		case '2': result += "card error"; break;
		case '3': result += "card inside (2)"; break;
		case '4': result += "card at the entrance"; break;
		default: result += "?"; break;
		}
		if(response[1] != '0')
		{
			result += string_format(", error '%c'", static_cast<char>(response[1]));
		}
		switch(response[2])
		{
		case '0': result += ", done"; break;
		case '2': result += ", cannot execute"; break;
		case '3': result += ", in progress"; break;
		case '4': result += ", waiting for a card"; break;
		case '5': result += ", no card in the dispenser"; break;
		default: break;
		}
		return result;
	}

	//Writes one block of the card reader log: what is answered and what it means
	void LogCardReply(const std::vector<uint8>& reply)
	{
		if((reply.size() == 1) && (reply[0] == UART_ACK))
		{
			CardLog(">> %s (acknowledged)", FormatHexDump(reply.data(), static_cast<uint32>(reply.size()), 40).c_str());
		}
		else if((reply.size() >= 8) && (reply[1] == 6))
		{
			CardLog(">> %s\r\n", FormatHexDump(reply.data(), static_cast<uint32>(reply.size()), 40).c_str());
			CardLog("   response %c%c%c: %s\r\n", static_cast<char>(reply[3]), static_cast<char>(reply[4]),
			        static_cast<char>(reply[5]), DescribeCardResponse(reply.data() + 3).c_str());
		}
		else
		{
			CardLog(">> %s\r\n", FormatHexDump(reply.data(), static_cast<uint32>(reply.size()), 40).c_str());
			CardLog("   card data, %d bytes\r\n", static_cast<int>(reply.size()));
		}
		CardLog("\r\n");
	}

	//Splits a card frame: 02 LEN CMD <3 ASCII digits> <data> 03 CHK
	//LEN counts the bytes from itself through the end of text marker, so the marker is at
	//index LEN and the three response bytes follow the command byte.
	bool ParseCardFrame(const uint8* frame, uint32 size, uint8& command, uint32& blockId, const uint8** payload, uint32& payloadSize)
	{
		if(size < 8) return false;
		if(frame[0] != 0x02) return false;
		uint32 length = frame[1];
		if((length + 2) > size) return false;
		if(frame[length] != 0x03) return false;

		command = frame[2];
		blockId = 0;
		for(uint32 i = 0; i < 3; i++)
		{
			uint8 character = frame[3 + i];
			if((character < '0') || (character > '9'))
			{
				blockId = 0;
				break;
			}
			blockId = (blockId * 10) + (character - '0');
		}

		*payload = frame + 6;
		payloadSize = (length >= 6) ? (length - 6) : 0;
		return true;
	}

	uint32 ParseAsciiNumber(const uint8* data, uint32 size)
	{
		uint32 result = 0;
		for(uint32 i = 0; i < size; i++)
		{
			if((data[i] < '0') || (data[i] > '9')) return 0;
			result = (result * 10) + (data[i] - '0');
		}
		return result;
	}

	uint32 ReadUint32(const std::vector<uint8>& data, size_t& offset)
	{
		if((offset + 4) > data.size()) return 0;
		uint32 result = data[offset] | (data[offset + 1] << 8) | (data[offset + 2] << 16) | (data[offset + 3] << 24);
		offset += 4;
		return result;
	}

	void AppendUint32(std::vector<uint8>& data, uint32 value)
	{
		data.push_back(static_cast<uint8>(value));
		data.push_back(static_cast<uint8>(value >> 8));
		data.push_back(static_cast<uint8>(value >> 16));
		data.push_back(static_cast<uint8>(value >> 24));
	}
}

bool CArcadeSpy::m_serialPassthrough = false;
void* CArcadeSpy::m_serialHandle = nullptr;

CArcadeSpy::CArcadeSpy(std::string moduleId, uint8* iopRam)
    : m_moduleId(moduleId)
    , m_iopRam(iopRam)
{
	m_uartReplyPath = CAppConfig::GetInstance().GetBasePath() / UART_REPLY_FILE;
	m_cardPath = CAppConfig::GetInstance().GetBasePath() / "arcadecards";
	m_cardControlPath = CAppConfig::GetInstance().GetBasePath() / "arcade_card.txt";
	m_serialConfigPath = CAppConfig::GetInstance().GetBasePath() / "arcade_serial.txt";

	//The serial port is shared: whichever instance opens it first is used by all of them
	OpenSerialPortIfConfigured();

	//The cards of the store are not read here: they are read when one is actually needed
	//("take=0", "insert=N" or the automatic mode). Reading them while the reader starts up
	//would keep around the data of a card that was changed in the meantime.
}

std::string CArcadeSpy::GetId() const
{
	return m_moduleId;
}

std::string CArcadeSpy::GetFunctionName(unsigned int functionId) const
{
	auto functionName = GetFunctionNameFor(m_moduleId, functionId);
	return functionName ? std::string(functionName) : string_format("unknown_%d", static_cast<int>(functionId));
}

bool CArcadeSpy::TryLoadUartReplyFile()
{
	std::error_code errorCode;
	if(!fs::exists(m_uartReplyPath, errorCode)) return false;
	auto writeTime = fs::last_write_time(m_uartReplyPath, errorCode);
	if(errorCode) return false;
	if(m_uartReplyFileLoaded && (writeTime == m_uartReplyFileTime)) return false;

	m_uartCommands.clear();
	m_uartCommandSearchStart = 0;
	m_uartSelectedCommand = static_cast<size_t>(-1);
	m_uartSelectedReplyIndex = 0;
	m_uartReadQueue.clear();
	m_uartReplyFileLoaded = true;
	m_uartReplyFileTime = writeTime;

	try
	{
		auto stream = Framework::CreateInputStdStream(m_uartReplyPath.native());
		uint64 size = stream.GetLength();
		std::vector<char> contents(static_cast<size_t>(size) + 1);
		stream.Read(contents.data(), static_cast<uint32>(size));
		contents[static_cast<size_t>(size)] = 0;

		//Parse the lines first: a serial capture log holds both directions, while a plain
		//file just lists what the device connected to the port sends.
		struct PARSED_LINE
		{
			bool fromDevice = false;
			std::vector<uint8> bytes;
		};
		std::vector<PARSED_LINE> lines;
		{
			std::string currentLine;
			auto flushLine = [&]() {
				if(currentLine.empty()) return;
				PARSED_LINE line;
				line.fromDevice = (currentLine.find("DEV->CRW") != std::string::npos);
				line.bytes = ParseHexBytes(currentLine);
				if(!line.bytes.empty()) lines.push_back(line);
				currentLine.clear();
			};

			for(char character : contents)
			{
				if((character == '\r') || (character == '\n'))
				{
					flushLine();
				}
				else if(character != 0)
				{
					currentLine += character;
				}
			}
			flushLine();
		}

		bool hasRequests = false;
		for(const auto& line : lines)
		{
			if(line.fromDevice && ((line.bytes.size() != 1) || (line.bytes[0] != UART_ENQ)))
			{
				hasRequests = true;
				break;
			}
		}

		if(hasRequests)
		{
			for(const auto& line : lines)
			{
				if(line.fromDevice)
				{
					//Anything but the ENQ byte starts a new command
					if((line.bytes.size() != 1) || (line.bytes[0] != UART_ENQ))
					{
						UART_COMMAND_ENTRY entry;
						entry.request = line.bytes;
						m_uartCommands.push_back(entry);
					}
				}
				else if(!m_uartCommands.empty())
				{
					if((line.bytes.size() == 1) && (line.bytes[0] == UART_ACK))
					{
						m_uartCommands.back().hasAck = true;
					}
					else
					{
						m_uartCommands.back().replies.push_back(line.bytes);
					}
				}
			}
		}
		else
		{
			//A plain list of replies is answered to any command, in order
			UART_COMMAND_ENTRY entry;
			entry.hasAck = false;
			for(const auto& line : lines)
			{
				entry.replies.push_back(line.bytes);
			}
			if(!entry.replies.empty()) m_uartCommands.push_back(entry);
		}

		size_t replyCount = 0;
		for(const auto& command : m_uartCommands)
		{
			replyCount += command.replies.size();
		}
		CardLog( "uart reply file loaded: %d command(s), %d reply frame(s)\r\n",
		                         static_cast<int>(m_uartCommands.size()), static_cast<int>(replyCount));
	}
	catch(...)
	{
		CardLog( "failed to load uart reply file.\r\n");
		return false;
	}
	return true;
}

bool CArcadeSpy::ShouldLogUartCommand()
{
	//The games poll the card reader continuously, so only a few entries are produced
	m_uartCommandLogCount++;
	return (m_uartCommandLogCount <= 8) || ((m_uartCommandLogCount % 1024) == 0);
}

void CArcadeSpy::QueueUartCommand(const uint8* command, uint32 size)
{
	//Pass through mode: the bytes go to the real reader and its answers are handed back to
	//the game untouched, which is what makes a capture of the real exchanges possible
	if(m_serialPassthrough)
	{
		if(size >= 8)
		{
			CardLog("<< %s\r\n", FormatHexDump(command, size, 40).c_str());
			CardLog("   sent command 0x%02X (%s), %d bytes\r\n",
			        static_cast<uint32>(command[2]), GetCardCommandName(command[2]), static_cast<int>(size));
		}
		else if(size != 0)
		{
			CardLog("<< %02X (single byte)\r\n", static_cast<uint32>(command[0]));
		}
		WriteSerialPort(command, size);
		return;
	}

	//Every write coming from the game is dumped here, whatever its size: the short ones are
	//not command frames but they are part of the protocol (handshakes and the like) and have
	//to be visible in the log as well.
	/*
	if(size != 0)
	{
		CardLog("<< %s\r\n", FormatHexDump(command, size, 40).c_str());
		if(size >= 8)
		{
			CardLog("   command 0x%02X (%s), %d bytes\r\n",
			        static_cast<uint32>(command[2]), GetCardCommandName(command[2]), static_cast<int>(size));
		}
		else
		{
			CardLog("   short write, %d byte(s) = %s\r\n",
			        static_cast<int>(size), FormatHexDump(command, size, 8).c_str());
		}
	}*/

	//A single 0x06 is the game acknowledging a frame of the reader: it carries no answer
	if((size == 1) && (command[0] == 0x06))
	{
		CardLog("<< 06 (acknowledge)\r\n");
		return;
	}

	//The commands that were captured on a real reader follow the protocol exactly: the frame
	//is acknowledged with 0x06 and the result is fetched by the game with 0x05
	if(HandleModelledCardCommand(command, size)) return;

	//Everything else is answered from the captured reply file, so the modelled command the
	//game was waiting on is over
	m_cardCommand.active = false;

	//Anything else is dumped here and answered from the captured reply file
	if(size != 0)
	{
		CardLog("<< %s\r\n", FormatHexDump(command, size, 40).c_str());
	}

	HandleCardRequest(command, size);

	std::vector<uint8> request(command, command + size);

	//Find the next capture entry holding the same request, wrapping around when the end
	//of the file is reached. This keeps repeated requests (the games poll the reader for
	//its status a lot) answered with the proper reply instead of drifting to the replies
	//of the commands that follow in the capture.
	size_t entryIndex = static_cast<size_t>(-1);
	for(size_t i = 0; i < m_uartCommands.size(); i++)
	{
		size_t index = (m_uartCommandSearchStart + i) % m_uartCommands.size();
		if(m_uartCommands[index].request == request)
		{
			entryIndex = index;
			break;
		}
	}
	if(entryIndex == static_cast<size_t>(-1))
	{
		//Fall back to a plain reply list (no request associated to the replies)
		for(size_t i = 0; i < m_uartCommands.size(); i++)
		{
			if(m_uartCommands[i].request.empty())
			{
				entryIndex = i;
				break;
			}
		}
	}
	if(entryIndex == static_cast<size_t>(-1))
	{
		//The capture does not know this command. Answer it like a real reader would, otherwise
		//the game never gets an acknowledge or a reply and reports an error. This happens for
		//the commands that are only used while dispensing a card.
		uint8 unknownCommand = (request.size() > 2) ? request[2] : 0;
		m_useSyntheticReply = true;
		m_syntheticReply = BuildSyntheticCardReply(unknownCommand);
		//Acknowledge every complete frame, even when no reply is known for it: a real reader
		//does that too, and the game would otherwise wait for the acknowledge forever.
		bool isCompleteFrame = (request.size() >= 8) && (request[0] == 0x02) && (request[1] == (request.size() - 2));
		if(isCompleteFrame && (m_uartReadQueue.size() < MAX_READ_QUEUE_SIZE))
		{
			m_uartReadQueue.push_back(UART_ACK);
		}
		if(ShouldLogUartCommand())
		{
			CardLog( "  command 0x%02X is not in the reply file: %s\r\n",
			                         static_cast<uint32>(unknownCommand), m_syntheticReply.empty() ? "no reply" : "synthetic reply");
		}
		return;
	}
	m_useSyntheticReply = false;

	m_uartSelectedCommand = entryIndex;
	m_uartSelectedReplyIndex = 0;
	m_uartCommandSearchStart = (entryIndex + 1) % m_uartCommands.size();

	if(m_uartCommands[entryIndex].hasAck && (m_uartReadQueue.size() < MAX_READ_QUEUE_SIZE))
	{
		m_uartReadQueue.push_back(UART_ACK);
	}
	if(ShouldLogUartCommand())
	{
		CardLog( "  command matched entry #%d (%d reply frame(s))\r\n",
		                         static_cast<int>(entryIndex), static_cast<int>(m_uartCommands[entryIndex].replies.size()));
	}
}

void CArcadeSpy::QueueUartReply()
{
	//Pass through mode: the request to resend the response goes to the real reader, nothing
	//is answered by the emulator
	if(m_serialPassthrough)
	{
		CardLog("<< 05 (resend request)\r\n");
		WriteSerialPort(&UART_ENQ, 1);
		return;
	}

	//The request to resend the response comes from the game as a single byte and is handled
	//here, so it has to be dumped here as well: it never reaches QueueUartCommand
	CardLog("<< 05 (resend request)\r\n");

	//The reply of the command the game is waiting on is rebuilt here: it changes while the
	//command runs (a card being inserted, a mechanism finishing), which is exactly what the
	//games expect from a real reader.
	if(m_cardCommand.active)
	{
		std::vector<uint8> reply = BuildCardCommandReply();
		if(!reply.empty() && ((m_uartReadQueue.size() + reply.size()) <= MAX_READ_QUEUE_SIZE))
		{
			m_uartReadQueue.insert(m_uartReadQueue.end(), reply.begin(), reply.end());
		}
		//LogCardReply(reply);
		return;
	}

	if(m_useSyntheticReply)
	{
		if((m_uartReadQueue.size() + m_syntheticReply.size()) <= MAX_READ_QUEUE_SIZE)
		{
			m_uartReadQueue.insert(m_uartReadQueue.end(), m_syntheticReply.begin(), m_syntheticReply.end());
		}
		LogCardReply(m_syntheticReply);
		return;
	}

	if(m_uartSelectedCommand >= m_uartCommands.size()) return;
	const auto& entry = m_uartCommands[m_uartSelectedCommand];
	if(entry.replies.empty()) return;

	//Send the next reply of the current command, or send the last one again when the
	//capture ran out of replies for it.
	size_t replyIndex = m_uartSelectedReplyIndex;
	if(replyIndex >= entry.replies.size())
	{
		replyIndex = entry.replies.size() - 1;
	}
	else
	{
		m_uartSelectedReplyIndex++;
	}

	std::vector<uint8> reply = entry.replies[replyIndex];
	HandleCardReply(reply);
	if((m_uartReadQueue.size() + reply.size()) > MAX_READ_QUEUE_SIZE) return;
	m_uartReadQueue.insert(m_uartReadQueue.end(), reply.begin(), reply.end());
	if(ShouldLogUartCommand())
	{
		LogCardReply(reply);
	}
}

uint32 CArcadeSpy::ServeUartRead(uint32 bufferPtr, uint32 size)
{
	//Pass through mode has to be handled first: the queue of the emulated reader is always
	//empty in that mode and the function used to return before ever reaching the real port,
	//so the reader was never read from.
	if(m_serialPassthrough)
	{
		static uint32 enteredReads = 0;
		enteredReads++;

		uint32 readSize = ReadSerialPort(m_iopRam + bufferPtr, size);
		if((readSize != 0) || (enteredReads <= 8) || ((enteredReads % 8192) == 0))
		{
			CardLog(">> read attempt %d: %d byte(s)\r\n",
			        static_cast<int>(enteredReads), static_cast<int>(readSize));
		}
		if(readSize != 0)
		{
			LogCardReply(std::vector<uint8>(m_iopRam + bufferPtr, m_iopRam + bufferPtr + readSize));
		}
		return readSize;
	}
	if(!IsValidRamRange(bufferPtr, size) || (size == 0)) return 0;

	if(m_uartReadQueue.empty())
	{
		TryLoadUartReplyFile();
		if(m_uartReadQueue.empty()) return 0;
	}

	uint32 transferSize = (static_cast<uint32>(m_uartReadQueue.size()) > size) ? size : static_cast<uint32>(m_uartReadQueue.size());
	memcpy(m_iopRam + bufferPtr, m_uartReadQueue.data(), transferSize);
	{
		//Tells whether the read path is reached at all and whether the shared pass through
		//flag is set for this instance: both have to be true for the real reader to be used
		static uint32 enteredReads = 0;
		enteredReads++;
		if((enteredReads <= 8) || ((enteredReads % 8192) == 0))
		{
			CardLog(">> ServeUartRead #%d, pass through %s\r\n",
			        static_cast<int>(enteredReads), m_serialPassthrough ? "on" : "off");
		}
	}

	//Pass through mode: the bytes of the real reader are handed to the game as they arrive,
	//and the emulated reader is not used at all
	if(m_serialPassthrough)
	{
		uint32 readSize = ReadSerialPort(m_iopRam + bufferPtr, size);

		//Reads are passive on Windows: an empty read transfers nothing and is invisible to a
		//port monitor. The attempts are counted here so that a port that is never read can be
		//told apart from one that is read but has nothing to give.
		static uint32 readAttempts = 0;
		readAttempts++;
		if((readSize != 0) || (readAttempts <= 8) || ((readAttempts % 8192) == 0))
		{
			CardLog(">> read attempt %d: %d byte(s)\r\n",
			        static_cast<int>(readAttempts), static_cast<int>(readSize));
		}

		if(readSize != 0)
		{
			LogCardReply(std::vector<uint8>(m_iopRam + bufferPtr, m_iopRam + bufferPtr + readSize));
		}
		return readSize;
	}

	//The control file is checked here as well: the games sometimes only read from the port
	//without writing anything, and the player must be able to move a card at any time
	UpdateCardControl();

	//Log what the game actually receives: every reply goes through here, whichever path
	//queued it, so this is the authoritative record of the exchange
	if(transferSize >= 1)
	{
		LogCardReply(std::vector<uint8>(m_uartReadQueue.begin(), m_uartReadQueue.begin() + transferSize));
	}
	m_uartReadQueue.erase(m_uartReadQueue.begin(), m_uartReadQueue.begin() + transferSize);
	return transferSize;
}

void CArcadeSpy::OpenSerialPortIfConfigured()
{
	//The port is shared by all instances: the first one that opens it wins, the others just
	//use it. Trying to open it again would fail because a serial port is exclusive.
	if(m_serialPassthrough) return;
#ifdef _WIN32
	std::error_code errorCode;
	if(!fs::exists(m_serialConfigPath, errorCode)) return;

	std::string contents;
	try
	{
		auto stream = Framework::CreateInputStdStream(m_serialConfigPath.native());
		uint64 size = stream.GetLength();
		contents.resize(static_cast<size_t>(size));
		if(size != 0)
		{
			stream.Read(&contents[0], static_cast<uint32>(size));
		}
	}
	catch(...)
	{
		CardLog("card: failed to read '%s', the reader is emulated\r\n", m_serialConfigPath.string().c_str());
		return;
	}

	//First line is the port name, the second one an optional baud rate
	std::string portName;
	uint32 baudRate = 9600;
	{
		size_t lineStart = 0;
		int lineIndex = 0;
		while(lineStart <= contents.size())
		{
			size_t lineEnd = contents.find_first_of("\r\n", lineStart);
			if(lineEnd == std::string::npos) lineEnd = contents.size();
			std::string line = contents.substr(lineStart, lineEnd - lineStart);
			while(!line.empty() && ((line.back() == ' ') || (line.back() == '\t'))) line.pop_back();
			size_t firstCharacter = line.find_first_not_of(" \t");
			if(firstCharacter != std::string::npos)
			{
				line = line.substr(firstCharacter);
				if(lineIndex == 0)
				{
					portName = line;
				}
				else if(lineIndex == 1)
				{
					try
					{
						baudRate = static_cast<uint32>(std::stoul(line));
					}
					catch(...)
					{
					}
				}
				lineIndex++;
			}
			if(lineEnd >= contents.size()) break;
			lineStart = lineEnd + 1;
		}
	}

	if(portName.empty())
	{
		CardLog("card: '%s' does not name a serial port, the reader is emulated\r\n", m_serialConfigPath.string().c_str());
		return;
	}

	std::string deviceName = "\\\\.\\" + portName;
	HANDLE handle = CreateFileA(deviceName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
	                            OPEN_EXISTING, 0, nullptr);
	if(handle == INVALID_HANDLE_VALUE)
	{
		CardLog("card: could not open '%s' (error %d), the reader is emulated\r\n",
		        portName.c_str(), static_cast<int>(GetLastError()));
		return;
	}

	DCB dcb = {};
	dcb.DCBlength = sizeof(DCB);
	if(!GetCommState(handle, &dcb))
	{
		CloseHandle(handle);
		CardLog("card: could not read the settings of '%s', the reader is emulated\r\n", portName.c_str());
		return;
	}

	dcb.BaudRate = baudRate;
	dcb.ByteSize = 8;
	dcb.Parity = NOPARITY;
	dcb.StopBits = ONESTOPBIT;
	dcb.fBinary = TRUE;
	dcb.fParity = FALSE;
	dcb.fOutxCtsFlow = FALSE;
	dcb.fOutxDsrFlow = FALSE;
	dcb.fDtrControl = DTR_CONTROL_ENABLE;
	dcb.fDsrSensitivity = FALSE;
	dcb.fOutX = FALSE;
	dcb.fInX = FALSE;
	dcb.fRtsControl = RTS_CONTROL_ENABLE;

	if(!SetCommState(handle, &dcb))
	{
		CloseHandle(handle);
		CardLog("card: could not configure '%s', the reader is emulated\r\n", portName.c_str());
		return;
	}

	//Non blocking reads: the game polls the port and must not be held up by the reader
	COMMTIMEOUTS timeouts = {};
	timeouts.ReadIntervalTimeout = MAXDWORD;
	timeouts.ReadTotalTimeoutMultiplier = 0;
	timeouts.ReadTotalTimeoutConstant = 0;
	timeouts.WriteTotalTimeoutMultiplier = 0;
	timeouts.WriteTotalTimeoutConstant = 100;
	SetCommTimeouts(handle, &timeouts);

	//Drop whatever the reader sent while the game was starting up
	PurgeComm(handle, PURGE_RXCLEAR | PURGE_TXCLEAR);

	m_serialHandle = handle;
	m_serialPassthrough = true;
	CardLog("card: pass through to the real reader on %s (%d baud)\r\n",
	        portName.c_str(), static_cast<int>(baudRate));
#endif
}

uint32 CArcadeSpy::ReadSerialPort(uint8* buffer, uint32 size)
{
#ifdef _WIN32
	if(m_serialHandle == nullptr) return 0;
	DWORD readCount = 0;
	if(!ReadFile(static_cast<HANDLE>(m_serialHandle), buffer, size, &readCount, nullptr)) return 0;
	return static_cast<uint32>(readCount);
#else
	return 0;
#endif
}

void CArcadeSpy::WriteSerialPort(const uint8* buffer, uint32 size)
{
#ifdef _WIN32
	if(m_serialHandle == nullptr) return;
	DWORD written = 0;
	WriteFile(static_cast<HANDLE>(m_serialHandle), buffer, size, &written, nullptr);
#endif
}

void CArcadeSpy::ScanCards()
{
	if(m_cardsScanned) return;
	m_cardsScanned = true;

	m_cardCount = 0;
	for(size_t i = 0; i < 64; i++)
	{
		auto path = m_cardPath / string_format("card%d.pbm", static_cast<int>(i));
		std::error_code errorCode;
		if(!fs::exists(path, errorCode)) break;
		m_cardCount++;
	}

	CardLog( "card store: %d card(s) in '%s'\r\n",
	                         static_cast<int>(m_cardCount), m_cardPath.string().c_str());
}

void CArcadeSpy::LoadCard(size_t index)
{
	m_cardBlocks.clear();
	m_cardImage.clear();
	m_cardTextFrames.clear();
	m_cardImagePrinted = false;

	auto path = m_cardPath / string_format("card%d.pbm", static_cast<int>(index));

	std::vector<uint8> contents;
	try
	{
		auto stream = Framework::CreateInputStdStream(path.native());
		uint64 size = stream.GetLength();
		contents.resize(static_cast<size_t>(size));
		if(size != 0)
		{
			stream.Read(contents.data(), static_cast<uint32>(size));
		}
	}
	catch(...)
	{
		CardLog( "failed to read card '%s'.\r\n", path.string().c_str());
		return;
	}

	//The artwork of the card is a P4 bitmap at the beginning of the file: "P4", the width, the
	//height and then the pixels (39 bytes per line without padding for the 312 pixels wide
	//picture). It has to be read back, otherwise a card that is only read and written would
	//lose its picture when it is saved again.
	size_t trailerOffset = 0;
	if((contents.size() > 10) && (contents[0] == 'P') && (contents[1] == '4'))
	{
		auto isSpace = [](uint8 character) {
			return (character == ' ') || (character == '\t') || (character == '\r') || (character == '\n');
		};
		size_t offset = 2;
		uint32 width = 0;
		uint32 height = 0;
		while((offset < contents.size()) && isSpace(contents[offset])) offset++;
		while((offset < contents.size()) && (contents[offset] >= '0') && (contents[offset] <= '9'))
		{
			width = (width * 10) + (contents[offset++] - '0');
		}
		while((offset < contents.size()) && isSpace(contents[offset])) offset++;
		while((offset < contents.size()) && (contents[offset] >= '0') && (contents[offset] <= '9'))
		{
			height = (height * 10) + (contents[offset++] - '0');
		}
		//Exactly one whitespace separates the header from the pixels
		if((offset < contents.size()) && isSpace(contents[offset])) offset++;
		size_t rowBytes = (width + 7) / 8;
		size_t imageSize = rowBytes * height;
		if((imageSize != 0) && ((offset + imageSize) <= contents.size()))
		{
			m_cardImage.assign(contents.begin() + offset, contents.begin() + offset + imageSize);
			trailerOffset = offset + imageSize;
			CardLog("card %d artwork: %d x %d pixels, %d bytes kept\r\n",
			        static_cast<int>(index), static_cast<int>(width), static_cast<int>(height),
			        static_cast<int>(imageSize));
		}
	}

	//The card data is hidden at the end of the image, behind a magic
	for(size_t i = trailerOffset; (i + sizeof(CARD_FILE_MAGIC)) <= contents.size(); i++)
	{
		if(memcmp(contents.data() + i, CARD_FILE_MAGIC, sizeof(CARD_FILE_MAGIC)) != 0) continue;

		size_t offset = i + sizeof(CARD_FILE_MAGIC);
		uint32 blockCount = ReadUint32(contents, offset);
		for(uint32 block = 0; block < blockCount; block++)
		{
			uint32 id = ReadUint32(contents, offset);
			uint32 blockSize = ReadUint32(contents, offset);
			if((offset + blockSize) > contents.size()) break;
			m_cardBlocks[id] = std::vector<uint8>(contents.begin() + offset, contents.begin() + offset + blockSize);
			offset += blockSize;
		}
		uint32 textSize = ReadUint32(contents, offset);
		if((offset + textSize) <= contents.size())
		{
			m_cardTextFrames.assign(contents.begin() + offset, contents.begin() + offset + textSize);
		}
		break;
	}

	CardLog( "card %d ready: %d data block(s)\r\n",
	                         static_cast<int>(index), static_cast<int>(m_cardBlocks.size()));
}

void CArcadeSpy::SaveCard()
{
	ScanCards();

	bool cardWasPresent = (m_cardIndex < m_cardCount);
	if(m_cardImage.empty() && m_cardBlocks.empty())
	{
		AdvanceCard();
		return;
	}

	if(!cardWasPresent)
	{
		//A new card was dispensed: it becomes the next card of the store and the one that
		//is presented to the game afterwards
		m_cardIndex = m_cardCount;
	}

	auto path = m_cardPath / string_format("card%d.pbm", static_cast<int>(m_cardIndex));

	std::vector<uint8> fileData;

	//Artwork: P4 bitmap of 312 pixels wide and 1 bit per pixel, 39 bytes per line. A card that
	//was printed keeps the printed picture, any other card keeps the picture it already had,
	//and a brand new card starts from a blank one of fifteen blocks.
	std::vector<uint8> image = m_cardImage;
	const char* artworkSource = "kept from the card file";
	if(m_cardImagePrinted)
	{
		artworkSource = "printed";
	}
	if(image.size() < CARD_IMAGE_BLANK_SIZE)
	{
		if(image.empty())
		{
			artworkSource = "blank card";
		}
		image.resize(CARD_IMAGE_BLANK_SIZE, 0);
	}
	uint32 imageHeight = static_cast<uint32>(image.size()) / CARD_IMAGE_ROW_SIZE;
	std::string header = string_format("P4\n%d %d\n", static_cast<int>(CARD_IMAGE_ROW_SIZE * 8), static_cast<int>(imageHeight));
	fileData.insert(fileData.end(), header.begin(), header.end());
	fileData.insert(fileData.end(), image.begin(), image.end());

	//Card data, hidden at the end of the image
	fileData.insert(fileData.end(), CARD_FILE_MAGIC, CARD_FILE_MAGIC + sizeof(CARD_FILE_MAGIC));
	AppendUint32(fileData, static_cast<uint32>(m_cardBlocks.size()));
	for(const auto& blockPair : m_cardBlocks)
	{
		AppendUint32(fileData, blockPair.first);
		AppendUint32(fileData, static_cast<uint32>(blockPair.second.size()));
		fileData.insert(fileData.end(), blockPair.second.begin(), blockPair.second.end());
	}
	AppendUint32(fileData, static_cast<uint32>(m_cardTextFrames.size()));
	fileData.insert(fileData.end(), m_cardTextFrames.begin(), m_cardTextFrames.end());

	try
	{
		Framework::PathUtils::EnsurePathExists(m_cardPath);
		auto stream = Framework::CreateOutputStdStream(path.native());
		stream.Write(fileData.data(), static_cast<uint32>(fileData.size()));
		CardLog( "card %d saved: '%s' (%d bytes, %d data block(s), artwork %s)\r\n",
		                         static_cast<int>(m_cardIndex), path.string().c_str(),
		                         static_cast<int>(fileData.size()), static_cast<int>(m_cardBlocks.size()),
		                         artworkSource);
		if(!cardWasPresent)
		{
			m_cardCount++;
		}
	}
	catch(...)
	{
		CardLog( "failed to write card '%s'.\r\n", path.string().c_str());
	}

	m_cardBlocks.clear();
	m_cardImage.clear();
	m_cardTextFrames.clear();
	m_cardImagePrinted = false;

	AdvanceCard();
}

void CArcadeSpy::AdvanceCard()
{
	ScanCards();
	if(m_cardCount == 0) return;

	//The game ejects a card, asks for another one and reads it again
	m_cardIndex = (m_cardIndex + 1) % m_cardCount;
	LoadCard(m_cardIndex);
	CardLog( "card reader: card %d inserted\r\n", static_cast<int>(m_cardIndex));
}

void CArcadeSpy::HandleCardRequest(const uint8* frame, uint32 size)
{
	uint8 command = 0;
	uint32 blockId = 0;
	const uint8* payload = nullptr;
	uint32 payloadSize = 0;
	if(!ParseCardFrame(frame, size, command, blockId, &payload, payloadSize)) return;

	//One block per command: what was received, what it is, what is answered to it
	CardLog("<< %s\r\n", FormatHexDump(frame, size, 40).c_str());
	CardLog("   command 0x%02X (%s), %d bytes, %d data bytes\r\n",
	        static_cast<uint32>(command), GetCardCommandName(command), static_cast<int>(size), static_cast<int>(payloadSize));

	//Report command codes the first time they are seen: this stays readable even when the
	//regular per command logging is throttled
	if(!m_seenCardCommands[command])
	{
		m_seenCardCommands[command] = true;
		CardLog( "card: command 0x%02X (%d byte frame)\r\n",
		                         static_cast<uint32>(command), static_cast<int>(size));
	}

	//The state machine is advanced by every command the game sends
	TickCardState();
	UpdateCardCommandState(command);

	switch(command)
	{
	case CARD_CMD_WRITE:
		if(payloadSize != 0)
		{
			m_cardBlocks[blockId] = std::vector<uint8>(payload, payload + payloadSize);
			CardLog( "  card write: block %d, %d bytes\r\n",
			                         static_cast<int>(blockId), static_cast<int>(payloadSize));
		}
		break;
	case CARD_CMD_WRITE_IMAGE:
		//00 00 00 | image id | 01 0D 01 04 | block index | 10 | data
		if(size >= 14)
		{
			//The block index counts across all of the artwork bands: the game sends the three
			//bands (17 blocks of 240 bytes each) as a single stream
			uint32 blockIndex = frame[12];
			uint32 dataSize = (size > 16) ? (size - 16) : 0;
			if(dataSize > CARD_IMAGE_BLOCK_SIZE) dataSize = CARD_IMAGE_BLOCK_SIZE;
			if(blockIndex != 0)
			{
				uint32 offset = (blockIndex - 1) * CARD_IMAGE_BLOCK_SIZE;
				if(m_cardImage.size() < (offset + CARD_IMAGE_BLOCK_SIZE))
				{
					m_cardImage.resize(offset + CARD_IMAGE_BLOCK_SIZE, 0);
				}
				memcpy(m_cardImage.data() + offset, frame + 14, dataSize);
			}
		}
		break;
	case CARD_CMD_WRITE_TEXT:
		m_cardTextFrames.insert(m_cardTextFrames.end(), frame + 6, frame + 1 + frame[1]);
		break;
	case CARD_CMD_EJECT:
		//The card is saved when there was one inside the reader: the state is '4' only in
		//that case. An empty discharge must not create a card out of nothing.
		if(m_cardState1 == '4')
		{
			SaveCard();
		}
		break;
	default:
		break;
	}
}

std::vector<uint8> CArcadeSpy::GetCardStatusReply(uint8 command)
{
	//A real reader answers every command with three response bytes: the position of the card,
	//the error code and the state of the command
	TickCardState();

	//Trace every change of the reported card position, with the command that caused it: this
	//is the only way to tell which step moved the card when no game command seems to do it
	static uint8 lastLoggedState1 = '?';
	if(lastLoggedState1 != m_cardState1)
	{
		lastLoggedState1 = m_cardState1;
		CardLog("card: card position is now '%c' (command 0x%02X)\r\n",
		        static_cast<char>(m_cardState1), static_cast<uint32>(command));
	}

	char response[4] = {static_cast<char>(m_cardState1), '0', static_cast<char>(m_cardState3), 0};
	return BuildCardCodeReply(command, response);
}

void CArcadeSpy::TickCardState()
{
	UpdateCardControl();

	//The games poll the reader, so its state advances with the polls
	if(m_cardBusyPolls != 0)
	{
		m_cardBusyPolls--;
		if(m_cardBusyPolls == 0)
		{
			m_cardState3 = '0';
		}
	}

	if(m_cardState1 == '4')
	{
		if(m_cardAuto && (m_cardSlotTakePolls != 0))
		{
			//The card was ejected and waits at the insertion slot. It is taken away
			//automatically, which is what makes the game move on to the next card.
			m_cardSlotPolls++;
			if(m_cardSlotPolls >= m_cardSlotTakePolls)
			{
				m_cardSlotPolls = 0;
				m_cardState1 = '0';
				CardLog( "card: card taken away from the slot\r\n");
			}
		}
	}
	else
	{
		m_cardSlotPolls = 0;
	}
}

void CArcadeSpy::UpdateCardControl()
{
	//The file is checked on every call: it holds a few bytes only and the player has to be
	//able to move a card without waiting. Checking it every 32 commands made the controls
	//(insert and take) feel like they took seconds to reach the game.
	m_cardControlCheckCounter++;

	std::error_code errorCode;
	if(!fs::exists(m_cardControlPath, errorCode)) return;
	auto writeTime = fs::last_write_time(m_cardControlPath, errorCode);
	if(errorCode) return;
	if(m_cardControlLoaded && (writeTime == m_cardControlTime)) return;
	m_cardControlLoaded = true;
	m_cardControlTime = writeTime;

	std::string contents;
	try
	{
		auto stream = Framework::CreateInputStdStream(m_cardControlPath.native());
		uint64 size = stream.GetLength();
		contents.resize(static_cast<size_t>(size));
		if(size != 0)
		{
			stream.Read(&contents[0], static_cast<uint32>(size));
		}
	}
	catch(...)
	{
		CardLog( "card: failed to read the control file\r\n");
		return;
	}

	std::string token;
	bool actionApplied = false;
	for(size_t i = 0; i <= contents.size(); i++)
	{
		char character = (i < contents.size()) ? contents[i] : ' ';
		if(character == '#')
		{
			while((i < contents.size()) && (contents[i] != '\n')) i++;
			character = ' ';
		}
		bool isSeparator = (character == ' ') || (character == '\t') || (character == '\r') ||
		                   (character == '\n') || (character == ',');
		if(isSeparator)
		{
			if(!token.empty())
			{
				bool isAction = (token == "take") || (token == "remove") || (token == "feed") ||
				                (token == "eject") || (token.compare(0, 7, "insert=") == 0) ||
				                (token.compare(0, 5, "take=") == 0);
				ApplyCardControlCommand(token);
				if(isAction) actionApplied = true;
				token.clear();
			}
			continue;
		}
		token += character;
	}

	if(actionApplied)
	{
		//Action commands only apply once: the file is rewritten with the settings alone, so
		//that a later change of the settings cannot replay a card movement.
		try
		{
			std::string settings = string_format("%s slot=%d dispenser=%d",
			                                     m_cardAuto ? "auto" : "manual",
			                                     static_cast<int>(m_cardSlotTakePolls),
			                                     static_cast<int>(m_cardDispenserCards));
			auto stream = Framework::CreateOutputStdStream(m_cardControlPath.native());
			stream.Write(settings.data(), static_cast<uint32>(settings.size()));
			CardLog("card: control file reset (%s)\r\n", settings.c_str());
		}
		catch(...)
		{
		}
	}
}

void CArcadeSpy::ApplyCardControlCommand(const std::string& command)
{
	std::string token = command;
	for(char& character : token)
	{
		if((character >= 'A') && (character <= 'Z')) character = character - 'A' + 'a';
	}

	if(token == "auto")
	{
		m_cardAuto = true;
		CardLog( "card: automatic mode\r\n");
	}
	else if(token == "manual")
	{
		m_cardAuto = false;
		CardLog( "card: manual mode\r\n");
	}
	else if(token == "feed")
	{
		if(m_cardDispenserCards != 0) m_cardDispenserCards--;
		m_cardState1 = '1';
		m_cardState3 = '0';
		CardLog( "card: card fed from the dispenser (%d left)\r\n",
		                         static_cast<int>(m_cardDispenserCards));
	}
	else if(token.find("insert=") == 0)
	{
		uint32 index = 0;
		if(ParseControlNumber(token.substr(7), index))
		{
			ScanCards();
			if(index < m_cardCount)
			{
				if((m_cardState1 == '1') || (m_cardState1 == '3'))
				{
					//A card is already inside the reader: it has to be taken away before
					//another one can be put at the insertion slot. Without this the position
					//would go back to "card at the entrance" while the card is inside.
					CardLog("card: a card is already inside the reader, insert ignored\r\n");
					return;
				}
				m_cardIndex = index;
				LoadCard(m_cardIndex);
				//The player puts the card at the insertion slot and the reader takes it in when
				//a command asks for it. Response 1 stays '0' (no card in the device) meanwhile,
				//because '4' means a card left behind at the slot in the SDK.
				m_cardPendingInsert = true;
				CardLog("card: card %d at the insertion slot, waiting to be taken in\r\n", static_cast<int>(index));
			}
			else
			{
				CardLog( "card: there is no card %d in the store\r\n", static_cast<int>(index));
			}
		}
	}
	else if(token == "eject")
	{
		m_cardState1 = '4';
		m_cardSlotPolls = 0;
		m_cardState3 = '0';
		CardLog( "card: card moved to the insertion slot\r\n");
	}
	else if((token == "take") || (token == "remove") || (token.compare(0, 5, "take=") == 0))
	{
		//"take=0" takes the card away and also selects the card of the store that is going to
		//be inserted next, so that a player can choose between several cards. The cards are
		//read here and never while the reader is starting up.
		if(token.compare(0, 5, "take=") == 0)
		{
			uint32 index = 0;
			if(ParseControlNumber(token.substr(5), index))
			{
				ScanCards();
				if(index < m_cardCount)
				{
					m_cardIndex = index;
					LoadCard(m_cardIndex);
					CardLog("card: card %d selected, it is the next one to be inserted\r\n",
					        static_cast<int>(index));
				}
				else
				{
					CardLog("card: there is no card %d in the store\r\n", static_cast<int>(index));
				}
			}
		}
		m_cardState1 = '0';
		m_cardSlotPolls = 0;
		m_cardState3 = '0';
		m_cardPendingInsert = false;
		CardLog( "card: card taken away from the slot\r\n");
	}
	else if(token.find("state1=") == 0)
	{
		//Forces the first response byte, to find out which card position a game expects
		uint32 value = 0;
		if(ParseControlNumber(token.substr(7), value))
		{
			m_cardState1 = static_cast<uint8>('0' + (value % 10));
		}
		CardLog("card: response 1 forced to '%c'\r\n", static_cast<char>(m_cardState1));
	}
	else if(token.find("state3=") == 0)
	{
		//Forces the third response byte, to find out which command state a game expects
		uint32 value = 0;
		if(ParseControlNumber(token.substr(7), value))
		{
			m_cardState3 = static_cast<uint8>('0' + (value % 10));
		}
		CardLog("card: response 3 forced to '%c'\r\n", static_cast<char>(m_cardState3));
	}
	else if(token.find("dispenser=") == 0)
	{
		uint32 value = 0;
		if(ParseControlNumber(token.substr(10), value)) m_cardDispenserCards = value;
	}
	else if(token.find("slot=") == 0)
	{
		uint32 value = 0;
		if(ParseControlNumber(token.substr(5), value)) m_cardSlotTakePolls = value;
	}
	else
	{
		CardLog( "card: unknown control command '%s'\r\n", command.c_str());
	}
}

void CArcadeSpy::UpdateCardCommandState(uint8 command)
{
	switch(command)
	{
	case 0x10: //Initialise
		CardLog("card: reader initialised (state cleared)\r\n");
		m_cardState1 = '0';
		m_cardState3 = '0';
		m_cardBusyPolls = 0;
		break;
	case 0xB0: //Take a card in from the dispenser
		if(m_cardDispenserCards != 0)
		{
			m_cardDispenserCards--;
			m_cardState1 = '1';
			//Machine side operations are instantaneous here, so the command is reported as
			//finished right away. Leaving it in progress makes the games retry it forever.
			m_cardBusyPolls = 0;
			m_cardState3 = '0';
			CardLog( "card: blank card taken from the dispenser (%d left)\r\n",
			                         static_cast<int>(m_cardDispenserCards));
		}
		else
		{
			//Response 3 = '5': there is no card in the dispenser
			m_cardState3 = '5';
		}
		break;
	case 0x80: //Eject the card to the insertion slot
	case 0x55: //Write data and discharge the card
	case 0x75: //Print and discharge the card
		//The card ends up at the insertion slot, but only when there was one inside the
		//reader in the first place: an empty discharge must not create a card out of nothing
		m_cardSlotPolls = 0;
		m_cardBusyPolls = 0;
		m_cardState3 = '0';
		m_cardState1 = ((m_cardState1 == '1') || (m_cardState1 == '3')) ? '4' : '0';
		CardLog("card: discharge: card %s inside the reader (command 0x%02X)\r\n",
		        (m_cardState1 == '4') ? "was" : "was not", static_cast<uint32>(command));
		CardLog( "card: card at the insertion slot (command 0x%02X)\r\n",
		                         static_cast<uint32>(command));
		break;
	case 0x33: //Read
	case 0x35: //Read data request
		if((m_cardState1 == '0') && m_cardAuto)
		{
			//Automatic mode also performs the player's action: one of the cards of the store
			//is inserted when the game asks for a card
			ScanCards();
			if(m_cardCount != 0)
			{
				LoadCard(m_cardIndex);
				//Automatic mode performs the player's action as well: the card waits at the
				//insertion slot until a command takes it in (response 1 stays '0' until then)
				m_cardPendingInsert = true;
				CardLog( "card: card %d inserted automatically\r\n",
				                         static_cast<int>(m_cardIndex));
			}
		}
		//The reader pulls the card in from the insertion slot before reading it, so the games
		//see the card moving from the entrance to the inside of the reader. The position is
		//logged whatever it is, which tells a read that does not reach this code apart from
		//one that does.
		CardLog("card: read command, card position is '%c'\r\n", static_cast<char>(m_cardState1));
		if(m_cardPendingInsert || (m_cardState1 == '4'))
		{
			//The reader pulls the card in from the insertion slot by itself. Response 1 goes
			//from '0' (no card in the device) to '1' and response 3 from '3' to '0'.
			m_cardPendingInsert = false;
			m_cardState1 = '1';
			m_cardState3 = '0';
			CardLog("card: card taken in from the insertion slot\r\n");
		}
		if(m_cardState1 == '0')
		{
			//Response 3 = '4': waiting for a card to be inserted
			m_cardState3 = '4';
		}
		else
		{
			//Machine side operations are instantaneous here, so the command is reported as
			//finished right away. Leaving it in progress makes the games retry it forever.
			m_cardBusyPolls = 0;
			m_cardState3 = '0';
		}
		break;
	case 0x53: //Write
	case 0x78: //Print setup
	case 0x7A: //Register font
	case 0x7B: //Print image
	case 0x7C: //Print text
	case 0x7D:
		//The game is working on a card and the machine feeds one from its dispenser. This is
		//machine side behaviour and is therefore always automatic.
		if(m_cardState1 == '0')
		{
			m_cardState1 = '1';
			CardLog( "card: card fed from the dispenser\r\n");
		}
		//Machine side operations are instantaneous here, so the command is reported as
		//finished right away. Leaving it in progress makes the games retry it forever.
		m_cardBusyPolls = 0;
		m_cardState3 = '0';
		break;
	case 0x40: //Cancel
		m_cardBusyPolls = 0;
		m_cardState3 = '0';
		break;
	default:
		break;
	}
}

//Commands of the card reader that are driven by the flow of the game itself. The replies
//follow a capture of a real reader: a command frame is acknowledged with 0x06 and the result
//is fetched by the game with 0x05, which is answered with the state of the command at that
//moment (a card being inserted changes the answer, so it is rebuilt on every request).
bool CArcadeSpy::HandleModelledCardCommand(const uint8* frame, uint32 size)
{
	uint8 command = 0;
	uint32 blockId = 0;
	const uint8* payload = nullptr;
	uint32 payloadSize = 0;
	if(!ParseCardFrame(frame, size, command, blockId, &payload, payloadSize)) return false;

	//Only the commands that were captured on a real reader are handled here, everything else
	//keeps using the captured reply file
	if((command != 0x10) && (command != 0x20) && (command != 0x33) && (command != 0x53) &&
	   (command != 0x78) && (command != 0x7B) && (command != 0x7C) && (command != 0x80) &&
	   (command != 0x90) && (command != 0xA0) && (command != 0xB0)) return false;

	m_cardCommand.command = command;
	m_cardCommand.enqCount = 0;
	m_cardCommand.active = true;
	for(uint32 i = 0; i < 3; i++)
	{
		m_cardCommand.parameter[i] = (i < payloadSize) ? payload[i] : static_cast<uint8>('0');
	}

	if(command == 0x7B)
	{
		//The artwork frames are long: only the header is dumped here, the data is summarised
		//by the image handler (one line per frame instead of a hex dump of 240 bytes)
		CardLog("<< %s\r\n", FormatHexDump(frame, 16, 16).c_str());
	}
	else
	{
		CardLog("<< %s\r\n", FormatHexDump(frame, size, 40).c_str());
	}
	CardLog("   command 0x%02X (%s), %d bytes, parameter \"%c%c%c\"\r\n",
	        static_cast<uint32>(command), GetCardCommandName(command), static_cast<int>(size),
	        m_cardCommand.parameter[0], m_cardCommand.parameter[1], m_cardCommand.parameter[2]);

	UpdateCardCommandState(command);

	//The commands that carry card data keep the side effects of the captured path: the data
	//of a write is kept (a following read answers it back) and the card is saved to its file
	//when it is discharged after having been inside the reader.
	switch(command)
	{
	case 0x53: //Write: the payload holds the three digits that address the data, then the data
		if(payloadSize != 0)
		{
			m_cardBlocks[blockId] = std::vector<uint8>(payload, payload + payloadSize);
			CardLog("  card write: block %d, %d bytes\r\n",
			        static_cast<int>(blockId), static_cast<int>(payloadSize));
		}
		break;
	case 0x7B: //Print image. Every frame is acknowledged on its own and carries a chunk of the
	           //artwork, and the frame says where that chunk belongs:
	           //  frame[10] = first line of the block (a line is 24 pixels high)
	           //  frame[11] = last line of the block
	           //  frame[12] = number of the chunk inside that block (1 based)
	           //  frame[13] = type of the block (0x10, 0x33 and 0x0C seen so far)
	           //The picture is 312 pixels wide, 1 bit per pixel and with no padding, so a line
	           //is 39 bytes and a block of 24 lines is 936 bytes.
		{
			if(size > 16)
			{
				uint32 firstLine = frame[10];
				uint32 chunkIndex = frame[12];
				//Everything up to the data, plus the end marker and the checksum
				uint32 dataSize = size - 16;
				if((firstLine != 0) && (chunkIndex != 0) && (dataSize != 0))
				{
					if(m_cardImage.size() < CARD_IMAGE_BLANK_SIZE)
					{
						//Printing writes over the blank artwork a new card starts with
						m_cardImage.resize(CARD_IMAGE_BLANK_SIZE, 0);
					}
					uint32 offset = ((firstLine - 1) * CARD_IMAGE_GROUP_SIZE) +
					                ((chunkIndex - 1) * CARD_IMAGE_CHUNK_SIZE);
					if(m_cardImage.size() < (offset + dataSize))
					{
						m_cardImage.resize(offset + dataSize, 0);
					}
					memcpy(m_cardImage.data() + offset, frame + 14, dataSize);
					m_cardImagePrinted = true;
					CardLog("   image: lines %d..%d, chunk %d, %d bytes at offset %d\r\n",
					        static_cast<int>(firstLine), static_cast<int>(frame[11]),
					        static_cast<int>(chunkIndex), static_cast<int>(dataSize),
					        static_cast<int>(offset));
				}
			}
		}
		break;
	case 0x7C: //Print text: the frame holds the text of the card
		m_cardTextFrames.insert(m_cardTextFrames.end(), frame + 6, frame + 1 + frame[1]);
		break;
	case 0x80: //Discharge
		if(m_cardState1 == '4')
		{
			SaveCard();
		}
		break;
	default:
		break;
	}

	//A real reader acknowledges every complete frame, the game then asks for the result
	if(m_uartReadQueue.size() < MAX_READ_QUEUE_SIZE)
	{
		m_uartReadQueue.push_back(UART_ACK);
	}
	return true;
}

std::vector<uint8> CArcadeSpy::BuildCardCommandReply()
{
	const uint8 command = m_cardCommand.command;
	m_cardCommand.enqCount++;

	//Every command that acts on the mechanism takes some time, so its first result is always
	//"executing" (response 1 = '1', response 3 = '3'); the requests that follow report what
	//the reader has done in the meantime.
	const bool isFirstReply = (m_cardCommand.enqCount == 1);
	const bool needsTime = (command == 0x10) || (command == 0x33) || (command == 0x53) ||
	                       (command == 0x7B) || (command == 0x7C) || (command == 0x80);

	if(needsTime && isFirstReply)
	{
		//Response 1 keeps reporting the position of the card, as the capture shows: a read that
		//has no card yet answers "003", the commands that find one in the reader answer "103"
		const char* executing = ((command == 0x33) && (m_cardState1 == '0')) ? "003" : "103";
		return BuildCardCodeReply(command, executing);
	}

	switch(command)
	{
	case 0x10: //Initialise
		return BuildCardCodeReply(command, "100");

	case 0x20: //Card state is answered with the position of the card right away
		{
			//The status command is instantaneous, so response 3 is always '0': a read that is
			//still waiting for a card must not leak its state into the polls of the game (the
			//capture answers "000" here, never "004").
			char response[4] = {static_cast<char>(m_cardState1), '0', '0', 0};
			return BuildCardCodeReply(command, response);
		}

	case 0x33: //Read
		{
			//"210": the card is taken in from the insertion slot. While the player has not
			//inserted one the reader answers "004" (waiting for a card), which is what makes
			//the games offer to insert one or to create a new one.
			const bool fromInsertionSlot = (m_cardCommand.parameter[0] == '2');
			if(fromInsertionSlot)
			{
				if(m_cardPendingInsert || (m_cardState1 == '4'))
				{
					//Response 1 '0' -> '1' (the card is taken in) and response 3 '3' -> '0'
					m_cardPendingInsert = false;
					m_cardState1 = '1';
					m_cardState3 = '0';
					CardLog("card: card taken in from the insertion slot\r\n");
				}
				if((m_cardState1 == '1') || (m_cardState1 == '3'))
				{
					return BuildCardCodeReply(command, "100");
				}
				return BuildCardCodeReply(command, "004");
			}

			if(m_cardState1 == '0')
			{
				return BuildCardCodeReply(command, "004");
			}

			//"010": the data of the card. The request addresses it with three digits which are
			//not part of it, and a real reader answers with 69 bytes here. The length of a
			//reply counts from the length byte up to the end of text marker, so a reply that
			//carries N data bytes has a length of 6 + N.
			std::vector<uint8> data;
			if(!m_cardBlocks.empty())
			{
				data = m_cardBlocks.begin()->second;
			}
			//The command that wrote the card carries the three digits that address the data in
			//front of it, while a real reader answers with the data bytes only
			if(data.size() > 69)
			{
				data.erase(data.begin(), data.begin() + 3);
			}
			std::vector<uint8> frame;
			frame.push_back(0x02);
			frame.push_back(static_cast<uint8>(data.size() + 6));
			frame.push_back(command);
			frame.push_back('1');
			frame.push_back('0');
			frame.push_back('0');
			frame.insert(frame.end(), data.begin(), data.end());
			frame.push_back(0x03);
			uint8 checksum = 0;
			for(size_t i = 1; i < frame.size(); i++) checksum ^= frame[i];
			frame.push_back(checksum);
			return frame;
		}

	case 0x53: //Write, printing and the print setup are reported as finished once done. The
	case 0x78: //print setup ("7 10000" in the capture) is answered as finished right away,
	case 0x7B: //the artwork of 0x7B and the text of 0x7C take some time and are reported as
	case 0x7C: //executing first by the generic handling above.
		return BuildCardCodeReply(command, "100");

	case 0xB0: //The dispenser has to deliver one of its cards to the reader
		{
			//First request: nothing is in the device yet and the mechanism is running, which the
			//capture answers with "003". The requests that follow report the card as being
			//inside the reader and the command as finished: "100".
			if(m_cardCommand.enqCount == 1)
			{
				//Response 3 '5' means that there is no card in the dispenser
				return BuildCardCodeReply(command, (m_cardDispenserCards == 0) ? "005" : "003");
			}
			return BuildCardCodeReply(command, "100");
		}

	case 0x90: //Storing a card in the dispenser: this reader does not have that function
		{
			//Response 3 '2' means that the command cannot be executed
			char response[4] = {static_cast<char>(m_cardState1), '0', '2', 0};
			return BuildCardCodeReply(command, response);
		}

	case 0xA0: //Cleaning: the card that is inside the reader is pushed out when it is done,
	           //which the capture of a real reader shows ("103" while cleaning, then "403",
	           //"400" while the card waits at the insertion slot and "000" once taken).
		{
			if(m_cardCommand.enqCount == 1)
			{
				return BuildCardCodeReply(command, "103");
			}
			if(m_cardCommand.enqCount == 2)
			{
				if((m_cardState1 == '1') || (m_cardState1 == '3'))
				{
					//The cleaning uses a special card without read/write data, so it is pushed
					//out without saving anything to a card file
					m_cardState1 = '4';
					CardLog("card: card discharged at the end of the cleaning\r\n");
				}
				if(m_cardState1 == '4')
				{
					return BuildCardCodeReply(command, "403");
				}
				return BuildCardCodeReply(command, "000");
			}
			if(m_cardState1 == '4')
			{
				return BuildCardCodeReply(command, "400");
			}
			return BuildCardCodeReply(command, "000");
		}

	case 0x80: //Discharge puts the card at the insertion slot, then the player takes it away
		if((m_cardState1 == '1') || (m_cardState1 == '3'))
		{
			m_cardState1 = '4';
			CardLog("card: card at the insertion slot (command 0x80)\r\n");
		}
		if(m_cardState1 == '4')
		{
			return BuildCardCodeReply(command, "400");
		}
		return BuildCardCodeReply(command, "000");

	default:
		return GetCardStatusReply(command);
	}
	//Unreachable, every case above returns, but it keeps the compiler happy
	return GetCardStatusReply(command);
}

std::vector<uint8> CArcadeSpy::BuildSyntheticCardReply(uint8 command)
{
	//Commands the capture file does not know are answered with the state of the reader: the
	//first response byte is the position of the card and the third one the state of the
	//command. Responses are fixed per command only in the sense that the command decides
	//what it does, the card position is always reported.
	return GetCardStatusReply(command);
}

void CArcadeSpy::HandleCardReply(std::vector<uint8>& reply)
{
	uint8 command = 0;
	uint32 blockId = 0;
	const uint8* payload = nullptr;
	uint32 payloadSize = 0;
	if(!ParseCardFrame(reply.data(), static_cast<uint32>(reply.size()), command, blockId, &payload, payloadSize)) return;

	if(command != 0x33)
	{
		//Every reply carries the position of the card in its first response byte, so that the
		//games see the same card state across the whole sequence of commands they send.
		if(payloadSize == 0)
		{
			reply = GetCardStatusReply(command);
		}
		return;
	}
	//No card in the reader: the read command reports that it is waiting for a card to be
	//inserted (response 1 = '0', response 3 = '4'), which is what makes the games offer to
	//insert a card or to create a new one. This also applies to the status replies of the
	//read command, otherwise the game would be told that a command is in progress.
	if(m_cardState1 == '0')
	{
		static const uint8 noCardFrame[] = {0x02, 0x06, 0x33, 0x30, 0x30, 0x34, 0x03, 0x02};
		reply.assign(noCardFrame, noCardFrame + sizeof(noCardFrame));
		return;
	}
	//A card is inside the reader: the read command answers with the magnetic data the
	//application is waiting for. This applies to the status frames of the capture as well,
	//otherwise the game would be told that a card is present but would never receive the
	//data of the card it asked to read.
	{
		std::vector<uint8> data;
		auto blockIterator = m_cardBlocks.find(blockId);
		if(blockIterator != m_cardBlocks.end())
		{
			data = blockIterator->second;
		}
		else if(!m_cardBlocks.empty())
		{
			data = m_cardBlocks.begin()->second;
		}
		else if(payloadSize > 0)
		{
			data.assign(payload, payload + payloadSize);
		}

		std::vector<uint8> frame;
		frame.push_back(0x02);
		frame.push_back(static_cast<uint8>(data.size() + 6));
		frame.push_back(0x33);
		frame.push_back('1');
		frame.push_back('0');
		frame.push_back('0');
		frame.insert(frame.end(), data.begin(), data.end());
		frame.push_back(0x03);
		uint8 checksum = 0;
		for(size_t i = 1; i < frame.size(); i++) checksum ^= frame[i];
		frame.push_back(checksum);
		reply = frame;
	}
	return;

	//Read replies carrying card data are rebuilt from the card currently in the reader
	std::vector<uint8> data;
	auto blockIterator = m_cardBlocks.find(blockId);
	if(blockIterator != m_cardBlocks.end())
	{
		data = blockIterator->second;
	}
	else
	{
		//No data for this block (blank card or no card): keep what the capture provides
		return;
	}

	std::vector<uint8> frame;
	frame.push_back(0x02);
	frame.push_back(static_cast<uint8>(data.size() + 6));
	frame.insert(frame.end(), reply.begin() + 2, reply.begin() + 6);
	frame.insert(frame.end(), data.begin(), data.end());
	frame.push_back(0x03);
	uint8 checksum = 0;
	for(size_t i = 1; i < frame.size(); i++) checksum ^= frame[i];
	frame.push_back(checksum);
	reply = frame;
}

void CArcadeSpy::Invoke(CMIPS& context, unsigned int functionId)
{
	uint32 arg0 = context.m_State.nGPR[CMIPS::A0].nV0;
	uint32 arg1 = context.m_State.nGPR[CMIPS::A1].nV0;
	uint32 arg2 = context.m_State.nGPR[CMIPS::A2].nV0;
	uint32 arg3 = context.m_State.nGPR[CMIPS::A3].nV0;

	uint32 callCount = ++m_callCounts[functionId];
	bool shouldLog = (callCount <= FIRST_LOG_COUNT) || ((callCount % LOG_INTERVAL) == 0);

	//Most arcade module functions that transfer data take a (buffer, size) pair.
	bool isBufferTransfer = (arg1 != 0) && (arg1 <= MAX_DUMP_SIZE) && IsValidRamRange(arg0, arg1);

	uint32 result = 0;

	//Arcade interface timing registers
	if(m_moduleId == "acdev")
	{
		switch(functionId)
		{
		case 4:
			m_memDelayReg = arg0;
			break;
		case 5:
			result = m_memDelayReg;
			break;
		case 6:
			m_ioDelayReg = arg0;
			break;
		case 7:
			result = m_ioDelayReg;
			break;
		}
	}
	//Arcade serial port (MAX232 UART) - this is where the card reader is connected
	else if(m_moduleId == "acuart")
	{
		if((functionId == 11) || (functionId == 12))
		{
			//Port attributes: a single word holding the baud rate
			if(IsValidRamRange(arg0, arg1) && (arg1 >= sizeof(uint32)))
			{
				uint32 speed = *reinterpret_cast<const uint32*>(m_iopRam + arg0);
				if(functionId == 12) m_uartSpeed = speed;
				CardLog( "  speed = %d (size = %d)\r\n",
				                         static_cast<int>(speed), static_cast<int>(arg1));
			}
		}
		else if(functionId == 6)
		{
			//acUartModuleStatus: report the module as running
			result = 1;
		}
		else if(functionId == 8)
		{
			result = ServeUartRead(arg0, arg1);
		}
		else if(functionId == 9)
		{
			if(IsValidRamRange(arg0, arg1) && (arg1 != 0))
			{
				if((arg1 == 1) && (m_iopRam[arg0] == UART_ENQ))
				{
					//The game asks the reader to send its reply
					QueueUartReply();
				}
				else
				{
					//A request frame
					QueueUartCommand(m_iopRam + arg0, arg1);
				}
			}
			result = arg1;
		}
	}

	if(shouldLog)
	{
		CardLog( "%s: function %d (%s) [call #%d, pc = 0x%08X]; args = [0x%08X, 0x%08X, 0x%08X, 0x%08X] -> 0x%08X\r\n",
		                         m_moduleId.c_str(), functionId, GetFunctionName(functionId).c_str(), callCount, context.m_State.nPC, arg0, arg1, arg2, arg3, result);
		if(isBufferTransfer)
		{
			CardLog( "  buffer @ 0x%08X (%d bytes) = %s\r\n",
			                         arg0, static_cast<int>(arg1), FormatHexDump(m_iopRam + arg0, arg1).c_str());
		}
	}

	context.m_State.nGPR[CMIPS::V0].nV0 = result;
}
