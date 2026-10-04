# Agent AI

[ภาษาไทย](#ภาษาไทย) | [English](#agent-ai)

## ภาษาไทย

Agent AI เป็นโปรแกรม AI แบบ command line ที่เขียนด้วย C++17 รองรับประวัติแชตและเครื่องมือจัดการไฟล์ใน sandbox

### ผู้ให้บริการ AI

| ค่า `LLM` | ตัวอย่าง `MODEL` |
| --- | --- |
| `Gemini` | `gemini-2.5-flash` |
| `OpenAI` | `gpt-4o-mini` |
| `Groq` | `llama-3.3-70b-versatile` |
| `OpenRouter` | `openai/gpt-4o-mini` |

ชื่อ provider ไม่แยกตัวพิมพ์เล็ก/ใหญ่ ส่วน `MODEL` ต้องเป็น model ID ที่บัญชีของคุณใช้งานได้ โดย OpenRouter มักต้องใส่ชื่อ provider นำหน้า เช่น `openai/gpt-4o-mini`

### สิ่งที่ต้องติดตั้ง

- C++ compiler ที่รองรับ C++17
- libcurl
- header ของ nlohmann/json

ตัวอย่าง build บน macOS ที่ติดตั้ง dependencies ผ่าน Homebrew:

```sh
g++ -std=c++17 main.cpp -I/opt/homebrew/include -lcurl -o app
```

### ตั้งค่า

สร้างไฟล์ `.env` ที่ root ของโปรเจกต์ โดยไฟล์นี้ถูก ignore จาก git เพื่อป้องกันการ commit API key:

```dotenv
API_KEY="YOUR_API_KEY"
LLM="Gemini"
MODEL="gemini-2.5-flash"
SANDBOX_PATH="sandbox"
MESSAGE_HISTORY=0000
```

หากใช้ provider อื่น ให้เปลี่ยน API key, `LLM` และ `MODEL` ให้ตรงกับค่ายนั้น ดูตัวอย่างเพิ่มเติมใน `.env.example` และอย่า commit `.env` หรือใส่ key จริงไว้ใน source code

`SANDBOX_PATH` กำหนดตำแหน่ง sandbox รองรับทั้ง path แบบ relative (อิงจากตำแหน่ง `.env`) และ absolute หากไม่ระบุจะใช้ `sandbox` และตัว sandbox ต้องไม่ใช่ symlink

`MESSAGE_HISTORY` กำหนดจำนวนคู่ข้อความ user/AI ที่เก็บเป็นบริบท เช่น `10` คือเก็บ 10 รอบล่าสุด หากไม่ระบุหรือเป็น `0`/`0000` จะไม่เก็บประวัติ แต่ system prompt ยังทำงานตามปกติ

### เริ่มใช้งาน

รันจาก root ของโปรเจกต์เพื่อให้โปรแกรมพบ `.env`:

```sh
./app
```

พิมพ์ `e` หรือ `-e` เพื่อออกจากโปรแกรม บทสนทนาจะถูกเก็บในหน่วยความจำระหว่างที่โปรแกรมทำงาน และเริ่มใหม่เมื่อปิดโปรแกรม

ข้อความใน terminal แยกป้าย `You` และ `AI`, ตัดบรรทัดตามความกว้างหน้าต่าง และคงรูปแบบ code block สีจะแสดงเฉพาะ interactive terminal และปิดได้ด้วย environment variable `NO_COLOR`

### เครื่องมือ Sandbox

ไฟล์ที่ AI สร้าง อ่าน แสดงรายการ หรือลบ จะถูกจำกัดไว้ใน sandbox:

- `.ls` แสดงรายชื่อไฟล์ปกติที่อยู่ตรงใน sandbox
- `.read: 'filename.ext'` อ่านไฟล์ใน sandbox
- `.rm: 'filename.ext'` ลบไฟล์ใน sandbox; ส่งหลายบรรทัดเพื่อลบหลายไฟล์ได้
- `.name: 'basename' .message: 'ข้อความ' .new ```extension` สร้างไฟล์ โดยใส่เนื้อหาใน code fence

ชื่อใน `.name` ต้องไม่มีนามสกุล เพราะนามสกุลมาจาก code fence โปรแกรมจะจัดการนามสกุลที่ซ้ำให้อัตโนมัติ งานที่ต้องสร้างหลายไฟล์สามารถส่ง `.name` command block หลายชุดในคำตอบเดียวได้ แต่ละไฟล์มี `.message` ของตัวเอง

ระบบปฏิเสธ path traversal, symlink, directory และ `main.cpp`; AI จะลบไฟล์ได้เมื่อผู้ใช้ร้องขอเท่านั้น

### License

โปรเจกต์นี้ใช้ MIT License ดูเงื่อนไขฉบับเต็มได้ที่ [LICENSE](LICENSE)

---

C++17 command-line AI agent with chat history and sandbox file tools. The selected provider is configured through `.env`.

## Supported providers

| `LLM` value | API | Example model |
| --- | --- | --- |
| `Gemini` | Google Gemini `generateContent` API | `gemini-2.5-flash` |
| `OpenAI` | OpenAI-compatible Chat Completions API | `gpt-4o-mini` |
| `Groq` | Groq Chat Completions API | `llama-3.3-70b-versatile` |
| `OpenRouter` | OpenRouter Chat Completions API | `openai/gpt-4o-mini` |

Provider names are case-insensitive. `MODEL` is sent to the selected provider as-is, so use a model ID enabled for your account. OpenRouter model IDs usually include the provider prefix, such as `openai/gpt-4o-mini`.

## Requirements

- C++17 compiler
- libcurl
- nlohmann/json headers

Example build command for macOS with Homebrew:

```sh
g++ -std=c++17 main.cpp -I/opt/homebrew/include -lcurl -o app
```

## Configuration

Create `.env` in the project root. It is ignored by git so API keys are not committed. Set one provider and its matching key/model:

```dotenv
API_KEY="YOUR_API_KEY"
LLM="Gemini"
MODEL="gemini-2.5-flash"
SANDBOX_PATH="sandbox"
MESSAGE_HISTORY=0000
```

To use another provider, change all three values together. For example, for Groq:

```dotenv
API_KEY="YOUR_GROQ_API_KEY"
LLM="Groq"
MODEL="llama-3.3-70b-versatile"
SANDBOX_PATH="sandbox"
```

See `.env.example` for the OpenAI and OpenRouter examples. Create API keys in the account for the chosen provider; keys are not interchangeable between providers. Do not commit `.env` or paste a real key into source code.

`SANDBOX_PATH` selects where AI file tools can read, write, and list files. Relative paths are resolved from the directory containing `.env`; absolute paths are also accepted. If omitted, it defaults to `sandbox`. The configured directory itself must be a real directory, not a symlink.

`MESSAGE_HISTORY` controls how many previous user/assistant exchanges are retained and sent as conversation context. One exchange is one user message plus one AI reply. Set it to a positive number (for example, `10`) to keep the latest exchanges. If omitted or set to `0` (including `0000`), no chat history is stored or sent; the system prompt still applies. Tool handoffs also count as exchanges.

## Run

Run the program from the project root so it can find `.env`:

```sh
./app
```

Enter `e` or `-e` to exit. Conversation history is kept in memory for the current process and is cleared when the program exits.

Terminal chat uses separate `You` and `AI` labels, wraps prose to the terminal width, preserves code blocks, and uses color only when stdout is an interactive terminal. Set `NO_COLOR` to disable ANSI colors.

## Sandbox tools

AI-created files are written only to `sandbox/`. The agent can request:
- `.ls` to list regular files directly inside the sandbox
- `.read: 'filename.ext'` to read a file directly inside the sandbox
- `.rm: 'filename.ext'` to delete regular files directly inside the sandbox; multiple `.rm` lines can delete multiple files in one response
- `.name: 'basename' .message: 'Shown after successful write' .new ```extension` followed by a real newline, the file contents, and a closing code fence to create a file

For tasks that need multiple files, the AI can return multiple `.name` command blocks in one response. Each block creates one file, and the app displays each file's `.message` after its write succeeds.

The `.name` value is an extensionless basename; the extension comes from the code fence. The writer also removes a repeated matching extension (for example, `index.html` plus an `html` code fence is saved as `index.html`, not `index.html.html`).

Paths outside the sandbox, directory traversal, symlinks, directories, and `main.cpp` are rejected. All `.rm` commands are parsed before deletion begins. The list, read, and delete operations do not access files outside the sandbox. The AI is instructed to delete files only when explicitly requested.

## Provider API details

Gemini uses Google's `generateContent` endpoint and passes the key as a query parameter. OpenAI, Groq, and OpenRouter use their respective Chat Completions endpoints with bearer-token authentication. Conversation history and the system prompt are translated to each provider's message format. Unsupported `LLM` values stop at startup with a configuration error.

## License

This project is licensed under the MIT License. See [LICENSE](LICENSE) for the full text.
