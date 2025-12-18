/**
 * Talking Ben Architecture Graph
 * Interactive system block diagram
 */

const nodes = [
    {
        id: 'uart',
        label: 'UART',
        x: 0.1, y: 0.3,
        connections: ['ddr3'],
        description: 'PC to FPGA data transfer',
        filename: 'uart_rx.sv',
        code: `// UART Receiver Module
module uart_rx #(
    parameter CLKS_PER_BIT = 868  // 100MHz / 115200
)(
    input  logic clk,
    input  logic rst,
    input  logic rx_serial,
    output logic rx_valid,
    output logic [7:0] rx_byte
);

    typedef enum logic [2:0] {
        IDLE, START, DATA, STOP
    } state_t;
    
    state_t state;
    logic [15:0] clk_count;
    logic [2:0]  bit_index;
    
    always_ff @(posedge clk) begin
        if (rst) begin
            state <= IDLE;
            rx_valid <= 0;
        end else begin
            case (state)
                IDLE: if (!rx_serial) state <= START;
                START: if (clk_count == CLKS_PER_BIT/2) 
                    state <= DATA;
                DATA: if (bit_index == 7) state <= STOP;
                STOP: begin
                    rx_valid <= 1;
                    state <= IDLE;
                end
            endcase
        end
    end
endmodule`
    },
    {
        id: 'ddr3',
        label: 'DDR3',
        x: 0.3, y: 0.5,
        connections: ['arbiter'],
        description: '128MB frame and audio storage',
        filename: 'memory_map.sv',
        code: `// DDR3 Memory Map
// Total: 128MB DDR3 Storage

localparam FRAME_SIZE = 320 * 240 * 2;  // RGB565
localparam AUDIO_SIZE = 44100 * 2;       // 1 sec mono

// Animation Frames (0x0000_0000 - 0x00FF_FFFF)
localparam IDLE_ADDR     = 32'h0000_0000;
localparam POKE_ADDR     = 32'h0010_0000;
localparam SCRATCH_ADDR  = 32'h0020_0000;
localparam PHONE_UP_ADDR = 32'h0030_0000;

// Audio Clips (0x0100_0000 - 0x01FF_FFFF)  
localparam AUDIO_YES_ADDR  = 32'h0100_0000;
localparam AUDIO_NO_ADDR   = 32'h0100_8000;
localparam AUDIO_EUGH_ADDR = 32'h0101_0000;
localparam AUDIO_HOHO_ADDR = 32'h0101_8000;

// Python script writes to these addresses
// FPGA reads based on FSM state`
    },
    {
        id: 'arbiter',
        label: 'Arbiter',
        x: 0.5, y: 0.5,
        connections: ['video', 'audio'],
        description: 'Serializes DDR3 access',
        filename: 'arbiter.sv',
        code: `// DDR3 Access Arbiter
// Prioritizes audio over video

module arbiter (
    input  logic clk, rst,
    input  logic video_req,
    input  logic audio_req,
    output logic video_grant,
    output logic audio_grant
);
    
    typedef enum logic [1:0] {
        IDLE, VIDEO, AUDIO
    } state_t;
    
    state_t state;
    
    always_ff @(posedge clk) begin
        if (rst) begin
            state <= IDLE;
        end else begin
            case (state)
                IDLE: begin
                    // Audio priority for smoother sound
                    if (audio_req)
                        state <= AUDIO;
                    else if (video_req)
                        state <= VIDEO;
                end
                VIDEO: state <= IDLE;
                AUDIO: state <= IDLE;
            endcase
        end
    end
    
    assign video_grant = (state == VIDEO);
    assign audio_grant = (state == AUDIO);
endmodule`
    },
    {
        id: 'mic',
        label: 'PDM Mic',
        x: 0.1, y: 0.7,
        connections: ['voice'],
        description: 'On-board microphone input',
        filename: 'pdm_decoder.sv',
        code: `// PDM Microphone Decoder
// Converts pulse density to amplitude

module pdm_decoder #(
    parameter SAMPLE_WINDOW = 4096
)(
    input  logic clk,
    input  logic pdm_data,
    input  logic pdm_clk_rising,
    output logic [11:0] amplitude,
    output logic sample_valid
);

    logic [12:0] one_count;
    logic [12:0] sample_count;
    
    always_ff @(posedge clk) begin
        if (pdm_clk_rising) begin
            if (sample_count == SAMPLE_WINDOW - 1) begin
                // Calculate amplitude as distance from midpoint
                amplitude <= (one_count > 2048) ? 
                    one_count - 2048 : 2048 - one_count;
                sample_valid <= 1;
                one_count <= pdm_data;
                sample_count <= 0;
            end else begin
                one_count <= one_count + pdm_data;
                sample_count <= sample_count + 1;
                sample_valid <= 0;
            end
        end
    end
endmodule`
    },
    {
        id: 'voice',
        label: 'Voice Detect',
        x: 0.3, y: 0.8,
        connections: ['fsm'],
        description: 'Triggers on speech detection',
        filename: 'voice_detect.sv',
        code: `// Voice Detection Module
// Waits for silence after speech

module voice_detect #(
    parameter THRESHOLD_MIN = 100,
    parameter THRESHOLD_MAX = 2000,
    parameter SILENCE_CYCLES = 100_000_000  // 1 second
)(
    input  logic clk, rst,
    input  logic [11:0] amplitude,
    input  logic sample_valid,
    output logic voice_detected
);

    logic speaking;
    logic [31:0] silence_counter;
    
    always_ff @(posedge clk) begin
        if (rst) begin
            speaking <= 0;
            voice_detected <= 0;
            silence_counter <= 0;
        end else if (sample_valid) begin
            if (amplitude > THRESHOLD_MIN && 
                amplitude < THRESHOLD_MAX) begin
                speaking <= 1;
                silence_counter <= 0;
            end else if (speaking) begin
                silence_counter <= silence_counter + 1;
                if (silence_counter >= SILENCE_CYCLES) begin
                    voice_detected <= 1;
                    speaking <= 0;
                end
            end
        end
        
        if (voice_detected) voice_detected <= 0;
    end
endmodule`
    },
    {
        id: 'fsm',
        label: 'Animation FSM',
        x: 0.5, y: 0.2,
        connections: ['arbiter', 'lfsr'],
        description: 'Game state controller',
        filename: 'animation_fsm.sv',
        code: `// Animation State Machine
typedef enum logic [3:0] {
    IDLE,
    POKE, SCRATCH, BELLY,     // Button animations
    PHONE_UP, PHONE_IDLE,     // Phone states
    RESP_YES, RESP_NO,        // Random responses
    RESP_EUGH, RESP_HOHO,
    RETURN_IDLE
} state_t;

state_t state, next_state;

always_ff @(posedge clk) begin
    if (rst) state <= IDLE;
    else state <= next_state;
end

always_comb begin
    next_state = state;
    case (state)
        IDLE: begin
            if (btn[1]) next_state = POKE;
            if (btn[2]) next_state = SCRATCH;
            if (btn[3]) next_state = BELLY;
            if (sw_phone) next_state = PHONE_UP;
        end
        PHONE_IDLE: begin
            if (voice_detected) begin
                case (lfsr_val[1:0])
                    2'b00: next_state = RESP_YES;
                    2'b01: next_state = RESP_NO;
                    2'b10: next_state = RESP_EUGH;
                    2'b11: next_state = RESP_HOHO;
                endcase
            end
            if (!sw_phone) next_state = RETURN_IDLE;
        end
        // ... animation complete -> return
    endcase
end`
    },
    {
        id: 'lfsr',
        label: 'LFSR',
        x: 0.7, y: 0.2,
        connections: [],
        description: 'Pseudo-random generator',
        filename: 'lfsr.sv',
        code: `// Linear Feedback Shift Register
// 16-bit LFSR for random responses

module lfsr #(
    parameter WIDTH = 16,
    parameter SEED = 16'hACE1
)(
    input  logic clk, rst,
    output logic [WIDTH-1:0] random_val
);

    logic [WIDTH-1:0] lfsr_reg;
    logic feedback;
    
    // Optimal taps for 16-bit LFSR
    // x^16 + x^14 + x^13 + x^11 + 1
    assign feedback = lfsr_reg[15] ^ lfsr_reg[13] ^ 
                      lfsr_reg[12] ^ lfsr_reg[10];
    
    always_ff @(posedge clk) begin
        if (rst)
            lfsr_reg <= SEED;
        else
            lfsr_reg <= {lfsr_reg[14:0], feedback};
    end
    
    assign random_val = lfsr_reg;
    
    // Use LSBs for 4 response options:
    // random_val[1:0] -> 0:YES, 1:NO, 2:EUGH, 3:HOHO
endmodule`
    },
    {
        id: 'video',
        label: 'Video Reader',
        x: 0.7, y: 0.4,
        connections: ['bram'],
        description: 'Reads frames from DDR3',
        filename: 'simple_reader.sv',
        code: `// Simple DDR3 Frame Reader
module simple_reader (
    input  logic clk, rst,
    input  logic start,
    input  logic [31:0] start_addr,
    input  logic grant,
    output logic request,
    output logic [31:0] rd_addr,
    input  logic [127:0] rd_data,
    input  logic rd_valid,
    output logic [15:0] pixel_out,
    output logic pixel_valid
);

    typedef enum logic [2:0] {
        IDLE, REQUEST, WAIT_GRANT, READ, COOLDOWN
    } state_t;
    
    state_t state;
    logic [31:0] addr;
    logic toggle_buffer;  // Ping-pong select
    
    always_ff @(posedge clk) begin
        case (state)
            IDLE: if (start) begin
                addr <= start_addr;
                state <= REQUEST;
            end
            REQUEST: begin
                request <= 1;
                if (grant) state <= READ;
            end
            READ: if (rd_valid) begin
                // Write to BRAM
                state <= COOLDOWN;
            end
            COOLDOWN: state <= IDLE;
        endcase
    end
endmodule`
    },
    {
        id: 'audio',
        label: 'Audio Player',
        x: 0.7, y: 0.6,
        connections: ['pwm'],
        description: 'Streams audio from DDR3',
        filename: 'audio_player.sv',
        code: `// Audio WAV Player
module audio_player (
    input  logic clk, rst,
    input  logic [3:0] clip_select,
    input  logic start,
    output logic [7:0] audio_sample,
    output logic sample_valid
);

    // Sample rate: 44.1kHz
    // 100MHz / 44100 ≈ 2268 cycles
    localparam SAMPLE_PERIOD = 2268;
    
    logic [11:0] sample_counter;
    logic [31:0] addr, end_addr;
    logic playing;
    
    // Address lookup based on clip
    always_comb begin
        case (clip_select)
            4'd0: begin addr = AUDIO_YES_ADDR; 
                        end_addr = AUDIO_YES_END; end
            4'd1: begin addr = AUDIO_NO_ADDR;
                        end_addr = AUDIO_NO_END; end
            4'd2: begin addr = AUDIO_EUGH_ADDR;
                        end_addr = AUDIO_EUGH_END; end
            4'd3: begin addr = AUDIO_HOHO_ADDR;
                        end_addr = AUDIO_HOHO_END; end
            default: addr = 0;
        endcase
    end
    
    always_ff @(posedge clk) begin
        if (sample_counter == SAMPLE_PERIOD - 1) begin
            sample_valid <= 1;
            sample_counter <= 0;
        end else begin
            sample_counter <= sample_counter + 1;
            sample_valid <= 0;
        end
    end
endmodule`
    },
    {
        id: 'bram',
        label: 'BRAM Buffer',
        x: 0.85, y: 0.4,
        connections: ['vga'],
        description: 'Ping-pong frame buffer',
        filename: 'frame_buffer.sv',
        code: `// Dual BRAM Frame Buffer (Ping-Pong)
module frame_buffer (
    input  logic clk,
    // Write port (from DDR3 reader)
    input  logic [16:0] wr_addr,
    input  logic [15:0] wr_data,
    input  logic wr_en,
    input  logic buffer_select,
    // Read port (to VGA)
    input  logic [16:0] rd_addr,
    output logic [15:0] rd_data
);

    // Two 320x240 RGB565 buffers
    logic [15:0] buffer_a [76800];
    logic [15:0] buffer_b [76800];
    
    // Write to one buffer
    always_ff @(posedge clk) begin
        if (wr_en) begin
            if (buffer_select)
                buffer_b[wr_addr] <= wr_data;
            else
                buffer_a[wr_addr] <= wr_data;
        end
    end
    
    // Read from the other buffer
    // Prevents visual tearing
    always_ff @(posedge clk) begin
        if (buffer_select)
            rd_data <= buffer_a[rd_addr];
        else
            rd_data <= buffer_b[rd_addr];
    end
endmodule`
    },
    {
        id: 'vga',
        label: 'VGA Out',
        x: 0.85, y: 0.6,
        connections: [],
        description: 'HDMI output via VGA timing',
        filename: 'vga_controller.sv',
        code: `// VGA Controller (640x480 @ 60Hz)
module vga_controller (
    input  logic clk_25mhz,
    input  logic rst,
    output logic [9:0] drawX, drawY,
    output logic hsync, vsync,
    output logic blank
);

    // 640x480 @ 60Hz timing
    localparam H_ACTIVE = 640;
    localparam H_FP = 16;
    localparam H_SYNC = 96;
    localparam H_BP = 48;
    localparam H_TOTAL = 800;
    
    localparam V_ACTIVE = 480;
    localparam V_FP = 10;
    localparam V_SYNC = 2;
    localparam V_BP = 33;
    localparam V_TOTAL = 525;
    
    logic [9:0] h_count, v_count;
    
    always_ff @(posedge clk_25mhz) begin
        if (h_count == H_TOTAL - 1) begin
            h_count <= 0;
            if (v_count == V_TOTAL - 1)
                v_count <= 0;
            else
                v_count <= v_count + 1;
        end else
            h_count <= h_count + 1;
    end
    
    assign drawX = h_count;
    assign drawY = v_count;
    assign hsync = ~(h_count >= H_ACTIVE + H_FP && 
                     h_count < H_ACTIVE + H_FP + H_SYNC);
    assign vsync = ~(v_count >= V_ACTIVE + V_FP && 
                     v_count < V_ACTIVE + V_FP + V_SYNC);
    assign blank = (h_count >= H_ACTIVE) || (v_count >= V_ACTIVE);
endmodule`
    },
    {
        id: 'pwm',
        label: 'PWM Audio',
        x: 0.85, y: 0.8,
        connections: [],
        description: 'Digital audio output',
        filename: 'audio_pwm.sv',
        code: `// Audio PWM Output
module audio_pwm (
    input  logic clk,
    input  logic rst,
    input  logic [7:0] sample,
    input  logic sample_valid,
    output logic pwm_out
);

    // 8-bit PWM at ~390kHz (100MHz / 256)
    logic [7:0] pwm_counter;
    logic [7:0] current_sample;
    
    always_ff @(posedge clk) begin
        if (rst) begin
            pwm_counter <= 0;
            current_sample <= 128;  // Midpoint (silence)
        end else begin
            pwm_counter <= pwm_counter + 1;
            
            if (sample_valid)
                current_sample <= sample;
        end
    end
    
    // PWM comparison
    assign pwm_out = (pwm_counter < current_sample);
    
    // Connect to audio jack via low-pass filter
    // RC filter: R=1k, C=100nF -> fc ≈ 1.6kHz
endmodule`
    }
];

// Canvas and state
let canvas, ctx;
let animationProgress = 0;
let hoveredNode = null;
let selectedNode = null;
let hasAnimated = false;

const colors = {
    bg: '#fafafa',
    node: '#ffffff',
    nodeBorder: '#e5e5e7',
    nodeHover: '#f0f0f2',
    nodeSelected: '#1d1d1f',
    text: '#1d1d1f',
    textLight: '#6e6e73',
    line: '#d1d1d6',
    lineActive: '#1d1d1f'
};

document.addEventListener('DOMContentLoaded', init);

function init() {
    canvas = document.getElementById('architecture-graph');
    if (!canvas) return;
    
    ctx = canvas.getContext('2d');
    
    resizeCanvas();
    window.addEventListener('resize', resizeCanvas);
    
    canvas.addEventListener('mousemove', handleMouseMove);
    canvas.addEventListener('click', handleClick);
    canvas.addEventListener('mouseleave', () => {
        hoveredNode = null;
        draw();
    });
    
    const observer = new IntersectionObserver((entries) => {
        entries.forEach(entry => {
            if (entry.isIntersecting && !hasAnimated) {
                hasAnimated = true;
                animateEntrance();
            }
        });
    }, { threshold: 0.3 });
    
    observer.observe(canvas);
    draw();
}

function resizeCanvas() {
    const container = canvas.parentElement;
    const rect = container.getBoundingClientRect();
    
    const dpr = window.devicePixelRatio || 1;
    canvas.width = rect.width * dpr;
    canvas.height = 500 * dpr;
    canvas.style.width = rect.width + 'px';
    canvas.style.height = '500px';
    
    ctx.scale(dpr, dpr);
    draw();
}

function animateEntrance() {
    const duration = 1200;
    const startTime = performance.now();
    
    function animate(currentTime) {
        const elapsed = currentTime - startTime;
        animationProgress = Math.min(elapsed / duration, 1);
        animationProgress = 1 - Math.pow(1 - animationProgress, 3);
        
        draw();
        
        if (elapsed < duration) {
            requestAnimationFrame(animate);
        }
    }
    
    requestAnimationFrame(animate);
}

function draw() {
    const width = canvas.width / (window.devicePixelRatio || 1);
    const height = canvas.height / (window.devicePixelRatio || 1);
    
    ctx.fillStyle = colors.bg;
    ctx.fillRect(0, 0, width, height);
    
    drawConnections(width, height);
    drawNodes(width, height);
}

function drawConnections(width, height) {
    ctx.lineWidth = 1.5;
    
    nodes.forEach((node, index) => {
        const nodeProgress = getNodeProgress(index);
        if (nodeProgress <= 0) return;
        
        const x1 = node.x * width;
        const y1 = node.y * height;
        
        node.connections.forEach(targetId => {
            const target = nodes.find(n => n.id === targetId);
            if (!target) return;
            
            const targetIndex = nodes.indexOf(target);
            const targetProgress = getNodeProgress(targetIndex);
            if (targetProgress <= 0) return;
            
            const x2 = target.x * width;
            const y2 = target.y * height;
            
            const isActive = (selectedNode && 
                (selectedNode.id === node.id || selectedNode.id === targetId));
            
            ctx.strokeStyle = isActive ? colors.lineActive : colors.line;
            ctx.globalAlpha = Math.min(nodeProgress, targetProgress) * (isActive ? 1 : 0.6);
            
            ctx.beginPath();
            ctx.moveTo(x1, y1);
            ctx.lineTo(x2, y2);
            ctx.stroke();
        });
    });
    
    ctx.globalAlpha = 1;
}

function drawNodes(width, height) {
    const nodeRadius = 42;
    
    nodes.forEach((node, index) => {
        const progress = getNodeProgress(index);
        if (progress <= 0) return;
        
        const x = node.x * width;
        const y = node.y * height;
        const scale = progress;
        
        ctx.save();
        ctx.translate(x, y);
        ctx.scale(scale, scale);
        
        const isHovered = hoveredNode === node;
        const isSelected = selectedNode === node;
        
        ctx.beginPath();
        ctx.arc(0, 0, nodeRadius, 0, Math.PI * 2);
        
        if (isSelected) {
            ctx.fillStyle = colors.nodeSelected;
        } else if (isHovered) {
            ctx.fillStyle = colors.nodeHover;
        } else {
            ctx.fillStyle = colors.node;
        }
        ctx.fill();
        
        ctx.strokeStyle = isSelected ? colors.nodeSelected : colors.nodeBorder;
        ctx.lineWidth = isSelected ? 2 : 1;
        ctx.stroke();
        
        ctx.fillStyle = isSelected ? '#ffffff' : colors.text;
        ctx.font = '500 11px Inter, sans-serif';
        ctx.textAlign = 'center';
        ctx.textBaseline = 'middle';
        ctx.fillText(node.label, 0, 0);
        
        ctx.restore();
        
        node._hitX = x;
        node._hitY = y;
        node._hitRadius = nodeRadius;
    });
}

function getNodeProgress(index) {
    const stagger = index * 0.06;
    const nodeProgress = (animationProgress - stagger) / (1 - stagger * nodes.length / (nodes.length + 1));
    return Math.max(0, Math.min(1, nodeProgress));
}

function handleMouseMove(e) {
    const rect = canvas.getBoundingClientRect();
    const x = e.clientX - rect.left;
    const y = e.clientY - rect.top;
    
    let found = null;
    for (const node of nodes) {
        if (node._hitX === undefined) continue;
        const dx = x - node._hitX;
        const dy = y - node._hitY;
        const dist = Math.sqrt(dx * dx + dy * dy);
        if (dist < node._hitRadius) {
            found = node;
            break;
        }
    }
    
    if (found !== hoveredNode) {
        hoveredNode = found;
        canvas.style.cursor = found ? 'pointer' : 'default';
        draw();
    }
}

function handleClick(e) {
    if (hoveredNode) {
        selectedNode = hoveredNode;
        showCodePanel(selectedNode);
        draw();
    }
}

function showCodePanel(node) {
    const panel = document.getElementById('code-panel');
    const title = document.getElementById('code-title');
    const filename = document.getElementById('code-filename');
    const content = document.getElementById('code-content');
    
    title.textContent = node.label;
    filename.textContent = node.filename;
    content.textContent = node.code;
    
    panel.classList.add('visible');
}

function closeCodePanel() {
    const panel = document.getElementById('code-panel');
    panel.classList.remove('visible');
    selectedNode = null;
    draw();
}

window.closeCodePanel = closeCodePanel;
