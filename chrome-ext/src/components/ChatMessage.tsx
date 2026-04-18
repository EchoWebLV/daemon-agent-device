import React from 'react';
import ReactMarkdown from 'react-markdown';
import remarkGfm from 'remark-gfm';
import type { Message } from '../types';

interface ChatMessageProps {
  message: Message;
}

export function ChatMessage({ message }: ChatMessageProps) {
  const isUser = message.role === 'user';

  return (
    <div className={`flex ${isUser ? 'justify-end' : 'justify-start'} mb-3`}>
      <div
        className={`max-w-[85%] rounded-2xl px-4 py-3 ${
          isUser
            ? 'bg-daemon-accent/15 text-daemon-text border border-daemon-accent/20'
            : 'bg-daemon-surface text-gray-200 border border-daemon-border'
        }`}
      >
        {isUser ? (
          <p className="text-sm leading-relaxed whitespace-pre-wrap">{message.content}</p>
        ) : (
          <div className="prose prose-invert prose-sm max-w-none prose-p:my-1 prose-headings:my-2 prose-pre:bg-black/40 prose-pre:border prose-pre:border-daemon-border prose-code:text-daemon-accent prose-code:font-mono prose-code:text-xs prose-a:text-daemon-accent prose-a:underline prose-img:rounded-lg prose-img:my-2 break-words overflow-hidden">
            <ReactMarkdown
              remarkPlugins={[remarkGfm]}
              components={{
                a: ({ href, children }) => (
                  <a href={href} target="_blank" rel="noopener noreferrer" className="text-daemon-accent underline break-all">
                    {children}
                  </a>
                ),
                img: ({ src, alt }) => (
                  <img src={src} alt={alt || ''} className="rounded-lg max-w-full my-2" loading="lazy" />
                ),
                p: ({ children }) => (
                  <p className="my-1 break-words">{children}</p>
                ),
              }}
            >{message.content}</ReactMarkdown>
          </div>
        )}
        {!isUser && message.cost !== undefined && message.cost > 0 && (
          <p className="text-[10px] text-daemon-dim font-mono mt-2 text-right">
            ${message.cost.toFixed(4)}
          </p>
        )}
      </div>
    </div>
  );
}

export function ThinkingIndicator() {
  return (
    <div className="flex justify-start mb-3">
      <div className="bg-daemon-surface border border-daemon-border rounded-2xl px-4 py-3">
        <div className="flex items-center gap-1.5">
          <div className="w-1.5 h-1.5 bg-daemon-accent rounded-full animate-bounce [animation-delay:0ms]" />
          <div className="w-1.5 h-1.5 bg-daemon-accent rounded-full animate-bounce [animation-delay:150ms]" />
          <div className="w-1.5 h-1.5 bg-daemon-accent rounded-full animate-bounce [animation-delay:300ms]" />
        </div>
      </div>
    </div>
  );
}
