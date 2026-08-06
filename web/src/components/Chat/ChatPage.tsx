import { useEffect } from 'react';
import { useTranslation } from 'react-i18next';
import { PlusIcon, ChatBubbleLeftRightIcon, ClockIcon, TrashIcon } from '@heroicons/react/24/outline';
import { useChatStore } from '../../store/useChatStore';
import { useAppStore } from '../../store/useAppStore';
import { MessageList } from './MessageList';
import { ChatInput } from './ChatInput';
import { ProgrammaticBanner } from '../Common/ProgrammaticBanner';

export function ChatPage() {
  const { messages, sendMessage, isStreaming, stopStreaming, newSession, sessions, loadSessions, switchSession, deleteSession, sessionId } = useChatStore();
  const { systemStatus, currentNamespace } = useAppStore();
  const namespaceReady = useAppStore((s) =>
    s.namespaces.some((ns) => ns.name === s.currentNamespace),
  );
  const { t } = useTranslation();

  const llmEnabled = systemStatus?.llm_enabled ?? true;

  useEffect(() => {
    if (!namespaceReady) return;
    loadSessions();
  }, [loadSessions, currentNamespace, namespaceReady]);

  return (
    <div className="flex h-full">
      {/* Session History Sidebar */}
      <div className="w-56 shrink-0 flex flex-col bg-bg border-r border-line bg-surface ">
        <div className="p-3 border-b border-line">
          <button
            onClick={newSession}
            className="w-full flex items-center justify-center gap-2 px-3 py-2 text-sm font-medium rounded-lg bg-cortrix-600 text-white hover:bg-cortrix-700 transition-colors"
          >
            <PlusIcon className="w-4 h-4" />
            {t('chat.newSession')}
          </button>
        </div>

        <div className="flex-1 overflow-y-auto py-2 px-2 space-y-1">
          {sessions.length === 0 ? (
            <div className="text-center py-8 text-xs text-muted">
              <ClockIcon className="w-6 h-6 mx-auto mb-2 opacity-40" />
              {t('chat.noHistory')}
            </div>
          ) : (
            sessions.map((s) => {
              const date = new Date(s.created_at);
              const timeStr = date.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
              const dateStr = date.toLocaleDateString();
              const displayTitle = s.title && s.title !== 'New Chat'
                ? s.title
                : `${t('chat.untitled', { defaultValue: 'Chat' })}`;
              return (
                <div
                  key={s.session_id}
                  className={`group relative w-full text-left px-3 py-2 rounded-lg text-xs transition-colors cursor-pointer ${
                    sessionId === s.session_id
                      ? 'bg-cortrix-100 text-cortrix-700 dark:bg-cortrix-800/30 dark:text-cortrix-400'
                      : 'text-muted hover:bg-surface2 text-muted hover:bg-surface2'
                  }`}
                  onClick={() => switchSession(s.session_id)}
                >
                  <div className="flex items-center gap-2">
                    <ChatBubbleLeftRightIcon className="w-3.5 h-3.5 shrink-0" />
                    <span className="truncate flex-1">{displayTitle}</span>
                    <button
                      onClick={(e) => {
                        e.stopPropagation();
                        if (confirm(t('chat.deleteConfirm', { defaultValue: 'Delete this session?' }))) {
                          deleteSession(s.session_id);
                        }
                      }}
                      className="hidden group-hover:block shrink-0 p-0.5 rounded hover:bg-red-100 hover:text-red-500 dark:hover:bg-red-900/30 dark:hover:text-red-400 transition-colors"
                      title={t('chat.delete', { defaultValue: 'Delete' })}
                    >
                      <TrashIcon className="w-3 h-3" />
                    </button>
                  </div>
                  <div className="text-[10px] text-muted mt-0.5 ml-5.5">
                    {dateStr} {timeStr} · {s.interaction_count ?? 0} {t('chat.messages', { defaultValue: 'messages' })}
                  </div>
                </div>
              );
            })
          )}
        </div>
      </div>

      {/* Chat Main Area */}
      <div className="flex flex-col flex-1 min-w-0">
        {/* Chat Header */}
        <div className="px-6 py-3 shrink-0 flex items-center justify-between border-b border-line bg-surface ">
          <div>
            <h1 className="text-lg font-bold text-txt">{t('chat.title')}</h1>
            <p className="text-xs text-muted">{t('chat.subtitle')}</p>
          </div>
        </div>

        {/* Retrieval Mode Banner */}
        {!llmEnabled && (
          <div className="px-6 py-2 text-sm text-center bg-amber/10 text-amber border-b border-amber/30">
            {t('chat.retrievalBanner')}
          </div>
        )}

        {/* Programmatic equivalent — drive chat with the Agent SDK */}
        <div className="px-6 pt-3">
          <ProgrammaticBanner
            comment={t('chat.sdkComment')}
            snippet={
              <>
                <span className="text-magma-h">client</span>.<span className="text-amber">agent</span>.
                <span className="text-amber">chat</span>(<span className="text-txt">message</span>=
                <span className="text-ok">&quot;…&quot;</span>, <span className="text-txt">stream</span>=
                <span className="text-ok">True</span>)
              </>
            }
          />
        </div>

        <MessageList messages={messages} isStreaming={isStreaming} />
        <ChatInput onSend={sendMessage} onStop={stopStreaming} disabled={isStreaming} isStreaming={isStreaming} />
      </div>
    </div>
  );
}
