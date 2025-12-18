/**
 * OS Architecture Graph - Interactive visualization
 * Obsidian-style knowledge graph with animated entrance and clickable nodes
 */

// Node and connection data for OS architecture
const nodes = [
    {
        id: 'boot',
        label: 'Boot',
        x: 0.15, y: 0.15,
        connections: ['kernel'],
        description: 'RISC-V assembly boot sequence',
        filename: 'start.s',
        code: `// Boot sequence - start.s
// Initialize stack and jump to kernel

.section .text.init
.global _start
_start:
    // Set up stack pointer
    la sp, _stack_top
    
    // Clear BSS section
    la t0, _bss_start
    la t1, _bss_end
clear_bss:
    sw zero, 0(t0)
    addi t0, t0, 4
    blt t0, t1, clear_bss
    
    // Jump to C kernel main
    call main
    
    // Halt if main returns
halt:
    wfi
    j halt`
    },
    {
        id: 'kernel',
        label: 'Kernel Init',
        x: 0.35, y: 0.15,
        connections: ['memory', 'thread', 'device'],
        description: 'Kernel initialization and setup',
        filename: 'main.c',
        code: `// Kernel main - main.c

void main(void) {
    // Initialize console for debug output
    console_init();
    kprintf("Illinix 391 booting...\\n");
    
    // Initialize memory subsystem
    memory_init();
    
    // Initialize interrupt controller
    intr_init();
    plic_init();
    
    // Initialize thread manager
    thrmgr_init();
    
    // Initialize device drivers
    device_init();
    
    // Mount filesystem
    ktfs_mount(blkdev);
    
    // Initialize process manager
    procmgr_init();
    
    // Start shell
    thread_spawn("shell", shell_main);
    
    // Enable interrupts and schedule
    intr_enable();
    thread_yield();
}`
    },
    {
        id: 'memory',
        label: 'Memory Manager',
        x: 0.2, y: 0.45,
        connections: ['process'],
        description: 'Sv39 paging and virtual memory',
        filename: 'memory.h',
        code: `// Memory management - memory.h

#define PAGE_SIZE (1UL << 12)  // 4KB pages

// Page table entry flags (Sv39)
#define PTE_V (1 << 0)  // Valid
#define PTE_R (1 << 1)  // Readable
#define PTE_W (1 << 2)  // Writable
#define PTE_X (1 << 3)  // Executable
#define PTE_U (1 << 4)  // User accessible

// Memory space tag (opaque handle)
typedef unsigned long mtag_t;

// Core memory functions
extern void memory_init(void);
extern mtag_t active_mspace(void);
extern mtag_t switch_mspace(mtag_t mtag);
extern mtag_t clone_active_mspace(void);

// Page mapping
extern void * map_page(uintptr_t vma, 
    void * pp, int rwxug_flags);
extern void * alloc_and_map_range(
    uintptr_t vma, size_t size, int flags);

// Page fault handler
extern int handle_umode_page_fault(
    struct trap_frame * tfr, uintptr_t vma);`
    },
    {
        id: 'thread',
        label: 'Thread Scheduler',
        x: 0.5, y: 0.35,
        connections: ['process'],
        description: 'Preemptive multitasking scheduler',
        filename: 'thread.h',
        code: `// Thread management - thread.h

struct thread_list {
    struct thread *head;
    struct thread *tail;
};

struct condition {
    const char *name;
    struct thread_list wait_list;
};

struct lock {
    struct condition released;
    struct thread *owner;
    int count;
};

// Thread lifecycle
extern int thread_spawn(
    const char *name,
    void (*entry)(void), ...);

extern void thread_yield(void);
extern int thread_join(int tid);
extern void thread_exit(void);

// Synchronization primitives
extern void condition_wait(
    struct condition *cond);
extern void condition_broadcast(
    struct condition *cond);

extern void lock_acquire(struct lock *lock);
extern void lock_release(struct lock *lock);`
    },
    {
        id: 'process',
        label: 'Process Manager',
        x: 0.35, y: 0.65,
        connections: ['syscall', 'filesystem'],
        description: 'Process abstraction and lifecycle',
        filename: 'process.h',
        code: `// Process management - process.h

#define PROCESS_IOMAX 16

struct process {
    int idx;      // Index into process table
    int tid;      // Thread ID
    mtag_t mtag;  // Memory space tag
    struct io * iotab[PROCESS_IOMAX];
};

// Process lifecycle
extern void procmgr_init(void);

extern int process_exec(
    struct io * exeio, 
    int argc, char ** argv);

extern int process_fork(
    const struct trap_frame * tfr);

extern void process_exit(void);

// Get current process
static inline struct process * 
current_process(void) {
    return running_thread_process();
}`
    },
    {
        id: 'device',
        label: 'Device Drivers',
        x: 0.7, y: 0.2,
        connections: ['filesystem'],
        description: 'UART, VIRTIO, PLIC drivers',
        filename: 'device.h',
        code: `// Device abstraction - device.h

// Device operations table
struct device_ops {
    int (*open)(struct device *);
    int (*close)(struct device *);
    long (*read)(struct device *, 
        void *buf, long len);
    long (*write)(struct device *, 
        const void *buf, long len);
    int (*ioctl)(struct device *, 
        int cmd, void *arg);
};

struct device {
    const char *name;
    struct device_ops *ops;
    void *priv;
};

// Device initialization
extern void device_init(void);
extern void console_init(void);
extern void virtio_blk_init(void);
extern void plic_init(void);`
    },
    {
        id: 'filesystem',
        label: 'KTFS Filesystem',
        x: 0.65, y: 0.5,
        connections: ['syscall'],
        description: 'Block-based filesystem with caching',
        filename: 'ktfs.h',
        code: `// KTFS Filesystem - ktfs.h

#define KTFS_BLKSZ 512
#define KTFS_NUM_DIRECT_DATA_BLOCKS 3

// Superblock structure
struct ktfs_superblock {
    uint32_t block_count;
    uint32_t bitmap_block_count;
    uint32_t inode_block_count;
    uint16_t root_directory_inode;
} __attribute__((packed));

// Inode with indirect blocks
struct ktfs_inode {
    uint32_t size;
    uint32_t flags;
    uint32_t block[3];   // Direct
    uint32_t indirect;   // Single indirect
    uint32_t dindirect[2]; // Double indirect
} __attribute__((packed));

// Filesystem operations
extern int ktfs_mount(struct io *io);
extern int ktfs_open(const char *name, 
    struct io **ioptr);
extern long ktfs_read(struct io *io, 
    void *buf, long len);
extern long ktfs_write(struct io *io, 
    const void *buf, long len);
extern int ktfs_create(const char *name);
extern int ktfs_delete(const char *name);`
    },
    {
        id: 'syscall',
        label: 'System Calls',
        x: 0.5, y: 0.85,
        connections: [],
        description: 'User-kernel interface via ecall',
        filename: 'scnum.h',
        code: `// System call numbers - scnum.h

// Process management
#define SYSCALL_EXIT    0
#define SYSCALL_FORK    1
#define SYSCALL_EXEC    2
#define SYSCALL_WAIT    3

// File operations  
#define SYSCALL_OPEN    4
#define SYSCALL_CLOSE   5
#define SYSCALL_READ    6
#define SYSCALL_WRITE   7
#define SYSCALL_CREATE  8
#define SYSCALL_DELETE  9

// I/O control
#define SYSCALL_IOCTL   10
#define SYSCALL_DUP     11
#define SYSCALL_PIPE    12

// Misc
#define SYSCALL_USLEEP  13
#define SYSCALL_PRINT   14

// Syscall handler (in syscall.c)
// Invoked via RISC-V ecall instruction
// Reads syscall number from a7 register
// Arguments in a0-a5, return in a0`
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
    
    // Set canvas size
    resizeCanvas();
    window.addEventListener('resize', resizeCanvas);
    
    // Mouse events
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
    
    // Account for device pixel ratio
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
        
        // Ease out cubic
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
    
    // Clear
    ctx.fillStyle = colors.bg;
    ctx.fillRect(0, 0, width, height);
    
    // Draw connections first (behind nodes)
    drawConnections(width, height);
    
    // Draw nodes
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
            
            // Determine if connection is active
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
        
        // Determine node state
        const isHovered = hoveredNode === node;
        const isSelected = selectedNode === node;
        
        // Draw node circle
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
        
        // Draw label
        ctx.fillStyle = isSelected ? '#ffffff' : colors.text;
        ctx.font = '500 12px Inter, sans-serif';
        ctx.textAlign = 'center';
        ctx.textBaseline = 'middle';
        ctx.fillText(node.label, 0, 0);
        
        ctx.restore();
        
        // Store hit area for mouse detection
        node._hitX = x;
        node._hitY = y;
        node._hitRadius = nodeRadius;
    });
}

function getNodeProgress(index) {
    // Stagger node appearance
    const stagger = index * 0.1;
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

// Make closeCodePanel available globally
window.closeCodePanel = closeCodePanel;
