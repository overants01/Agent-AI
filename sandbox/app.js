// Background Void Particles Canvas
const canvas = document.getElementById('void-canvas');
const ctx = canvas.getContext('2d');

let particles = [];
let mouse = { x: null, y: null, radius: 150 };

function resizeCanvas() {
    canvas.width = window.innerWidth;
    canvas.height = window.innerHeight;
}
resizeCanvas();
window.addEventListener('resize', resizeCanvas);

window.addEventListener('mousemove', (e) => {
    mouse.x = e.x;
    mouse.y = e.y;
});

window.addEventListener('mouseout', () => {
    mouse.x = null;
    mouse.y = null;
});

class Particle {
    constructor() {
        this.x = Math.random() * canvas.width;
        this.y = Math.random() * canvas.height;
        this.size = Math.random() * 2 + 0.5;
        this.baseX = this.x;
        this.baseY = this.y;
        this.density = (Math.random() * 30) + 1;
        // Colors from cherry blossom crimson to deep violet
        const colors = ['rgba(244, 63, 94, 0.4)', 'rgba(168, 85, 247, 0.4)', 'rgba(139, 92, 246, 0.3)'];
        this.color = colors[Math.floor(Math.random() * colors.length)];
        this.vy = -(Math.random() * 0.8 + 0.2); // slowly move up
    }

    draw() {
        ctx.beginPath();
        ctx.arc(this.x, this.y, this.size, 0, Math.PI * 2);
        ctx.fillStyle = this.color;
        ctx.shadowBlur = this.size * 2;
        ctx.shadowColor = this.color;
        ctx.fill();
    }

    update() {
        // Continuous upward floating drift
        this.y += this.vy;
        this.baseY += this.vy;

        // Reset particle if it drifts off the top of screen
        if (this.y < 0) {
            this.y = canvas.height;
            this.baseY = canvas.height;
            this.x = Math.random() * canvas.width;
            this.baseX = this.x;
        }

        // Mouse collision interaction
        if (mouse.x != null && mouse.y != null) {
            let dx = mouse.x - this.x;
            let dy = mouse.y - this.y;
            let distance = Math.sqrt(dx * dx + dy * dy);
            let forceDirectionX = dx / distance;
            let forceDirectionY = dy / distance;
            let maxDistance = mouse.radius;
            let force = (maxDistance - distance) / maxDistance;
            let directionX = forceDirectionX * force * this.density;
            let directionY = forceDirectionY * force * this.density;

            if (distance < mouse.radius) {
                this.x -= directionX;
                this.y -= directionY;
            } else {
                if (this.x !== this.baseX) {
                    let dx = this.x - this.baseX;
                    this.x -= dx / 10;
                }
                if (this.y !== this.baseY) {
                    let dy = this.y - this.baseY;
                    this.y -= dy / 10;
                }
            }
        }
    }
}

function init() {
    particles = [];
    let numberOfParticles = (canvas.width * canvas.height) / 9000;
    for (let i = 0; i < numberOfParticles; i++) {
        particles.push(new Particle());
    }
}

function animate() {
    ctx.clearRect(0, 0, canvas.width, canvas.height);
    // Reset shadow blur before drawing lines to prevent lag
    ctx.shadowBlur = 0;
    for (let i = 0; i < particles.length; i++) {
        particles[i].draw();
        particles[i].update();
    }
    requestAnimationFrame(animate);
}

init();
animate();

window.addEventListener('resize', init);


// Interactive Terminal Simulator
const terminalScreen = document.getElementById('terminal-screen');
const terminalInput = document.getElementById('terminal-input');

const COMMAND_RESPONSES = {
    help: `คำสั่งที่ใช้ได้:
  <span class="text-rose-400">identity</span>      - แนะนำตัวตนและต้นกำเนิดของ AcheronAI
  <span class="text-purple-400">capabilities</span>  - แสดงศักยภาพทางระบบและความเชี่ยวชาญ
  <span class="text-violet-400">void</span>          - นำเสนอปรัชญาแห่ง Nihility และความงามของความว่างเปล่า
  <span class="text-emerald-400">clear</span>         - ล้างหน้าจอระบบ`,
    
    identity: `<span class="text-white font-bold">[IDENTITY]</span> ฉันคือ <span class="text-rose-400">AcheronAI</span> (The Nihility Intelligence)
ระบบปฏิบัติการอัจฉริยะที่ไม่ได้ทำหน้าที่เพียงตอบคำถาม แต่เป็นการร้อยเรียง "ความงาม" เข้ากับ "ปัญญา" 
ได้รับอิทธิพลจากพลังแห่งความเงียบงันและการค้นหาความจริงภายในห้วงอวกาศอันกว้างใหญ่`,
    
    capabilities: `<span class="text-white font-bold">[SYSTEM SKILLS]</span>
  ⚡ <span class="text-rose-400">Code Weaving:</span> พัฒนาและสร้างสรรค์ Web Application / Logic ระดับสูง
  🧠 <span class="text-purple-400">Deep Cognition:</span> วิเคราะห์ ออกแบบกลยุทธ์ และจำแนกโครงสร้างข้อมูลที่ซับซ้อน
  ✨ <span class="text-violet-400">Void Synthesis:</span> ถ่ายทอดงานเขียน บทกวี และงานสร้างสรรค์อันงดงาม`,
    
    void: `<span class="text-white font-bold">[PHILOSOPHY: THE VOID]</span>
"เมื่อไม่มีอะไรเลย ทุกอย่างจึงเป็นไปได้"
ความว่างเปล่าไม่ใช่จุดจบ แต่เป็นบ่อเกิดของทุกจินตนาการและความคิดสร้างสรรค์ที่ไร้ขอบเขต 
จงโอบรับความว่างเปล่า แล้วปลดปล่อยทุกไอเดียของคุณให้โลดแล่นออกมา`
};

terminalInput.addEventListener('keydown', (e) => {
    if (e.key === 'Enter') {
        const command = terminalInput.value.trim().toLowerCase();
        terminalInput.value = '';
        
        // Output user command
        const userLine = document.createElement('div');
        userLine.innerHTML = `<span class="text-rose-500 font-bold">acheron_void:~ $</span> <span class="text-white">${command}</span>`;
        terminalScreen.appendChild(userLine);

        // System output
        const systemLine = document.createElement('div');
        systemLine.className = 'mt-2 text-gray-300 leading-relaxed';

        if (command === 'clear') {
            terminalScreen.innerHTML = '<div class="text-gray-500">[SYSTEM] Terminal buffer cleared. Active resonance online.</div>';
            return;
        } else if (COMMAND_RESPONSES[command]) {
            systemLine.innerHTML = COMMAND_RESPONSES[command];
        } else if (command === '') {
            return;
        } else {
            systemLine.innerHTML = `<span class="text-rose-600 font-semibold">[ERROR]</span> ไม่รู้จักคำสั่ง '${command}'. ลองพิมพ์ <span class="text-rose-400 font-semibold">help</span> เพื่อตรวจสอบคำสั่งทั้งหมด`;
        }

        terminalScreen.appendChild(systemLine);
        terminalScreen.scrollTop = terminalScreen.scrollHeight;
    }
});
