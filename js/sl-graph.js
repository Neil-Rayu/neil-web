/**
 * Sign Language Architecture Graph - Interactive visualization
 * Shows FPGA/Raspberry Pi system architecture with CNN pipeline
 */

// Node data for sign language system architecture
const nodes = [
    {
        id: 'camera',
        label: 'Camera',
        x: 0.12, y: 0.5,
        connections: ['preprocess'],
        description: 'USB camera input capturing hand gestures',
        filename: 'capture.py',
        code: `# Camera capture module
import cv2
import numpy as np

class CameraCapture:
    def __init__(self, device_id=0):
        self.cap = cv2.VideoCapture(device_id)
        self.cap.set(cv2.CAP_PROP_FPS, 80)
        self.cap.set(cv2.CAP_PROP_FRAME_WIDTH, 224)
        self.cap.set(cv2.CAP_PROP_FRAME_HEIGHT, 224)
    
    def capture_frame(self):
        ret, frame = self.cap.read()
        if ret:
            # Convert to grayscale for CNN
            gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
            return gray
        return None
    
    def release(self):
        self.cap.release()`
    },
    {
        id: 'preprocess',
        label: 'Preprocessing',
        x: 0.3, y: 0.5,
        connections: ['rpi'],
        description: 'Image normalization and formatting',
        filename: 'preprocess.cpp',
        code: `// Image preprocessing for CNN input
#include <stdint.h>

// Normalize pixel values to fixed-point
void preprocess_frame(
    uint8_t* input,      // Raw grayscale
    int16_t* output,     // Normalized output
    int width,
    int height
) {
    const int16_t SCALE = 256;  // Q8.8 format
    const int16_t MEAN = 128 * SCALE;
    
    for (int i = 0; i < width * height; i++) {
        // Normalize to [-1, 1] range
        int16_t pixel = (int16_t)input[i] * SCALE;
        output[i] = (pixel - MEAN) / 128;
    }
}`
    },
    {
        id: 'rpi',
        label: 'Raspberry Pi',
        x: 0.5, y: 0.3,
        connections: ['fpga'],
        description: 'Host processor coordinating system',
        filename: 'host_controller.cpp',
        code: `// Raspberry Pi host controller
#include "fpga_driver.h"
#include "camera.h"

class HostController {
private:
    FPGADriver& fpga;
    Camera& camera;
    
public:
    // Main inference loop
    void run_inference() {
        while (running) {
            // Capture frame
            auto frame = camera.capture();
            
            // Send to FPGA accelerator
            fpga.send_frame(frame.data());
            
            // Wait for result (non-blocking)
            auto result = fpga.receive_result();
            
            // Process classification
            int gesture = argmax(result, NUM_CLASSES);
            display_translation(gesture);
        }
    }
};`
    },
    {
        id: 'fpga',
        label: 'FPGA Accelerator',
        x: 0.5, y: 0.7,
        connections: ['conv', 'pool', 'fc'],
        description: 'Hardware CNN accelerator',
        filename: 'cnn_top.cpp',
        code: `// FPGA CNN Top-Level (Vivado HLS)
#include "cnn_layers.h"

#pragma HLS INTERFACE axis port=input
#pragma HLS INTERFACE axis port=output

void cnn_accelerator(
    hls::stream<pixel_t>& input,
    hls::stream<result_t>& output
) {
    #pragma HLS DATAFLOW
    
    // Layer buffers
    static fm_t conv1_out[CONV1_OUT_SIZE];
    static fm_t pool1_out[POOL1_OUT_SIZE];
    static fm_t conv2_out[CONV2_OUT_SIZE];
    static fm_t fc_out[NUM_CLASSES];
    
    // Pipelined CNN inference
    conv_layer<CONV1>(input, conv1_out);
    pool_layer<POOL1>(conv1_out, pool1_out);
    conv_layer<CONV2>(pool1_out, conv2_out);
    fc_layer(conv2_out, fc_out);
    
    // Output classification
    output_result(fc_out, output);
}`
    },
    {
        id: 'conv',
        label: 'Conv Layers',
        x: 0.7, y: 0.35,
        connections: ['output'],
        description: 'Optimized convolution with DSP',
        filename: 'conv_layer.cpp',
        code: `// Optimized Convolution Layer (HLS)
template<int PARAMS>
void conv_layer(
    fm_t input[IN_SIZE],
    fm_t output[OUT_SIZE]
) {
    #pragma HLS ARRAY_PARTITION variable=weights cyclic factor=8
    #pragma HLS PIPELINE II=1
    
    // 3x3 convolution with ReLU
    for (int oy = 0; oy < OUT_H; oy++) {
        for (int ox = 0; ox < OUT_W; ox++) {
            for (int oc = 0; oc < OUT_C; oc++) {
                #pragma HLS UNROLL factor=4
                
                acc_t sum = bias[oc];
                
                // Kernel loop
                for (int ky = 0; ky < 3; ky++) {
                    for (int kx = 0; kx < 3; kx++) {
                        for (int ic = 0; ic < IN_C; ic++) {
                            sum += input[...] * weights[...];
                        }
                    }
                }
                
                // ReLU activation
                output[...] = (sum > 0) ? sum : 0;
            }
        }
    }
}`
    },
    {
        id: 'pool',
        label: 'Pooling',
        x: 0.7, y: 0.55,
        connections: ['output'],
        description: 'Max pooling for downsampling',
        filename: 'pool_layer.cpp',
        code: `// Max Pooling Layer (HLS)
template<int PARAMS>
void pool_layer(
    fm_t input[IN_SIZE],
    fm_t output[OUT_SIZE]
) {
    #pragma HLS PIPELINE II=1
    
    // 2x2 max pooling with stride 2
    for (int oy = 0; oy < OUT_H; oy++) {
        for (int ox = 0; ox < OUT_W; ox++) {
            for (int c = 0; c < CHANNELS; c++) {
                fm_t max_val = MIN_VAL;
                
                // 2x2 window
                for (int py = 0; py < 2; py++) {
                    for (int px = 0; px < 2; px++) {
                        int iy = oy * 2 + py;
                        int ix = ox * 2 + px;
                        
                        fm_t val = input[idx(iy,ix,c)];
                        if (val > max_val) {
                            max_val = val;
                        }
                    }
                }
                
                output[idx(oy,ox,c)] = max_val;
            }
        }
    }
}`
    },
    {
        id: 'fc',
        label: 'Fully Connected',
        x: 0.7, y: 0.75,
        connections: ['output'],
        description: 'Classification layer',
        filename: 'fc_layer.cpp',
        code: `// Fully Connected Layer (HLS)
void fc_layer(
    fm_t input[FC_IN_SIZE],
    fm_t output[NUM_CLASSES]
) {
    #pragma HLS ARRAY_PARTITION variable=fc_weights cyclic factor=16
    #pragma HLS PIPELINE II=1
    
    // Matrix-vector multiplication
    for (int o = 0; o < NUM_CLASSES; o++) {
        #pragma HLS UNROLL factor=4
        
        acc_t sum = fc_bias[o];
        
        for (int i = 0; i < FC_IN_SIZE; i++) {
            sum += input[i] * fc_weights[o][i];
        }
        
        output[o] = sum;
    }
}

// Softmax done on CPU for simplicity
// Output is raw logits`
    },
    {
        id: 'output',
        label: 'Translation',
        x: 0.88, y: 0.5,
        connections: [],
        description: 'Gesture to text translation',
        filename: 'translation.py',
        code: `# Gesture translation output
import numpy as np

# ASL alphabet mapping
GESTURES = [
    'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H',
    'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P',
    'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X',
    'Y', 'Z', 'SPACE', 'DELETE'
]

def translate(logits):
    """Convert CNN output to gesture"""
    probs = softmax(logits)
    idx = np.argmax(probs)
    confidence = probs[idx]
    
    if confidence > THRESHOLD:
        return GESTURES[idx], confidence
    return None, 0.0

def softmax(x):
    exp_x = np.exp(x - np.max(x))
    return exp_x / exp_x.sum()`
    }
];

// Canvas and context
let canvas, ctx;
let animationProgress = 0;
let hoveredNode = null;
let selectedNode = null;
let hasAnimated = false;

// Colors
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

// Initialize on load
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
    
    // Use Intersection Observer to trigger animation when scrolled into view
    const observer = new IntersectionObserver((entries) => {
        entries.forEach(entry => {
            if (entry.isIntersecting && !hasAnimated) {
                hasAnimated = true;
                animateEntrance();
            }
        });
    }, {
        threshold: 0.3 // Trigger when 30% visible
    });
    
    observer.observe(canvas);
    
    // Initial draw (shows nothing until animation starts)
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
    const nodeRadius = 45;
    
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
    const stagger = index * 0.08;
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
