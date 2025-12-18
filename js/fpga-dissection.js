/**
 * FPGA Dissection Animation
 * Scroll-captured 3D board rotation with component callouts
 */

let container, board, components, scrollHint;
let scrollProgress = 0;
let animationComplete = false;
const SCROLL_DISTANCE = 300; // pixels of scroll to complete animation

document.addEventListener('DOMContentLoaded', init);

function init() {
    container = document.getElementById('dissection-container');
    board = document.getElementById('fpga-board');
    scrollHint = document.getElementById('scroll-hint');
    components = document.querySelectorAll('.component');
    
    if (!container) return;
    
    // Capture scroll events - must be non-passive to preventDefault
    window.addEventListener('wheel', handleWheel, { passive: false });
    window.addEventListener('touchstart', handleTouchStart, { passive: true });
    window.addEventListener('touchmove', handleTouchMove, { passive: false });
    
    // Initial state
    updateAnimation(0);
}

function shouldLockScroll() {
    if (animationComplete) return false;
    
    const rect = container.getBoundingClientRect();
    
    // Lock when:
    // - Top of container is at or above 150px from viewport top
    // - Bottom of container is still visible (at least 100px in viewport)
    const topInRange = rect.top <= 150;
    const bottomVisible = rect.bottom >= 100;
    
    return topInRange && bottomVisible;
}

function handleWheel(e) {
    // Check if we should lock on every wheel event
    if (shouldLockScroll()) {
        // Block ALL scrolling while locked
        e.preventDefault();
        
        // Only progress animation on scroll down
        if (e.deltaY > 0) {
            scrollProgress = Math.min(1, scrollProgress + e.deltaY / SCROLL_DISTANCE);
            updateAnimation(scrollProgress);
            
            if (scrollProgress >= 1) {
                animationComplete = true;
            }
        }
        return;
    }
    // Normal scrolling otherwise
}

let touchStartY = 0;

function handleTouchStart(e) {
    touchStartY = e.touches[0].clientY;
}

function handleTouchMove(e) {
    const touchY = e.touches[0].clientY;
    const deltaY = touchStartY - touchY;
    touchStartY = touchY;
    
    // Check if we should lock on every touch move
    if (shouldLockScroll()) {
        e.preventDefault();
        
        // Only progress animation on scroll down (swipe up)
        if (deltaY > 0) {
            scrollProgress = Math.min(1, scrollProgress + deltaY / SCROLL_DISTANCE);
            updateAnimation(scrollProgress);
            
            if (scrollProgress >= 1) {
                animationComplete = true;
            }
        }
    }
}

function updateAnimation(progress) {
    // Phase 1: Board rotation (0% - 40%)
    // Only rotate on X axis for straight "fold down" effect
    const rotationProgress = Math.min(progress / 0.4, 1);
    const rotateX = easeOutCubic(rotationProgress) * 65;
    const translateY = easeOutCubic(rotationProgress) * 80;
    const translateZ = easeOutCubic(rotationProgress) * -30;
    
    board.style.transform = `
        rotateX(${rotateX}deg) 
        translateY(${translateY}px)
        translateZ(${translateZ}px)
    `;
    
    // Phase 2: Components appear (30% - 100%)
    const componentProgress = Math.max(0, (progress - 0.3) / 0.7);
    
    components.forEach((comp, index) => {
        const delay = index * 0.1;
        const compProgress = Math.max(0, Math.min(1, (componentProgress - delay) / 0.3));
        
        if (compProgress > 0) {
            comp.classList.add('visible');
            const floatY = (1 - easeOutCubic(compProgress)) * 30;
            comp.style.transform = `translateY(${floatY}px) scale(${0.8 + 0.2 * easeOutCubic(compProgress)})`;
            comp.style.opacity = easeOutCubic(compProgress);
        } else {
            comp.classList.remove('visible');
            comp.style.opacity = 0;
        }
    });
    
    // Hide scroll hint after starting
    if (progress > 0.1) {
        scrollHint.classList.add('hidden');
    } else {
        scrollHint.classList.remove('hidden');
    }
}

function easeOutCubic(t) {
    return 1 - Math.pow(1 - t, 3);
}
