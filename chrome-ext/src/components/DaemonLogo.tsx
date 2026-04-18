import React, { useRef, useEffect, useCallback } from 'react';

interface DaemonLogoProps {
  isTyping: boolean;
  className?: string;
}

const FRAME_STEP = 1 / 30;
const REVERSE_INTERVAL = 1000 / 30;

export function DaemonLogo({ isTyping, className = 'w-40 h-40' }: DaemonLogoProps) {
  const videoRef = useRef<HTMLVideoElement>(null);
  const reverseTimerRef = useRef<number | null>(null);
  const prevTypingRef = useRef(false);

  const stopReverse = useCallback(() => {
    if (reverseTimerRef.current !== null) {
      clearInterval(reverseTimerRef.current);
      reverseTimerRef.current = null;
    }
  }, []);

  const playReverse = useCallback(() => {
    const video = videoRef.current;
    if (!video) return;
    video.pause();
    stopReverse();

    reverseTimerRef.current = window.setInterval(() => {
      if (!video) return;
      const next = video.currentTime - FRAME_STEP;
      if (next <= 0) {
        video.currentTime = 0;
        stopReverse();
        return;
      }
      video.currentTime = next;
    }, REVERSE_INTERVAL);
  }, [stopReverse]);

  useEffect(() => {
    if (isTyping === prevTypingRef.current) return;
    prevTypingRef.current = isTyping;

    const video = videoRef.current;
    if (!video) return;

    if (isTyping) {
      stopReverse();
      video.playbackRate = 1;
      video.play().catch(() => {});
    } else {
      playReverse();
    }
  }, [isTyping, stopReverse, playReverse]);

  useEffect(() => {
    const video = videoRef.current;
    if (!video) return;

    const handleEnded = () => {
      if (video.duration) video.currentTime = video.duration - 0.01;
    };
    video.addEventListener('ended', handleEnded);
    return () => {
      video.removeEventListener('ended', handleEnded);
      stopReverse();
    };
  }, [stopReverse]);

  return (
    <video
      ref={videoRef}
      src="icons/daemon-logo.mp4"
      className={`${className} object-contain`}
      style={{ mixBlendMode: 'lighten' }}
      muted
      playsInline
      preload="auto"
    />
  );
}
