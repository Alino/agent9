use std::cell::Cell;
use std::collections::VecDeque;
use std::io::Read;
use std::marker::PhantomData;
use std::sync::{mpsc, Mutex};
use std::time::Instant;

use smol_str::SmolStr;

use crate::error::EventLoopError;
use crate::event::{self, Modifiers, StartCause};
use crate::event_loop::{self, ControlFlow, DeviceEvents};
use crate::keyboard::{
    Key, KeyLocation, ModifiersKeys, ModifiersState, NamedKey, NativeKey, NativeKeyCode,
    PhysicalKey,
};
use crate::window::{CustomCursor as RootCustomCursor, CustomCursorSource, Theme, WindowId as RootWindowId};

use super::protocol::{self, Record};
use super::{DeviceId, KeyEventExtra, MonitorHandle, OsError, PlatformSpecificEventLoopAttributes, WindowId};

/// Everything that can wake the event loop travels through one channel, so
/// plain `recv`/`recv_timeout`/`try_recv` implement Wait/WaitUntil/Poll.
pub(super) enum Message {
    /// A record read from stdin by the reader thread.
    Record(Record),
    /// A user event is waiting in the (separate, typed) user channel.
    UserWake,
    /// `Window::request_redraw` (possibly from another thread).
    Redraw(WindowId),
    /// The window struct was dropped.
    Destroyed(WindowId),
}

fn rune_to_named(rune: u32) -> Option<NamedKey> {
    Some(match rune {
        protocol::KBS => NamedKey::Backspace,
        protocol::KTAB => NamedKey::Tab,
        protocol::KNL | 0x0D => NamedKey::Enter,
        protocol::KESC => NamedKey::Escape,
        protocol::KSPACE => NamedKey::Space,
        protocol::KDEL => NamedKey::Delete,
        protocol::KDOWN => NamedKey::ArrowDown,
        protocol::KUP => NamedKey::ArrowUp,
        protocol::KLEFT => NamedKey::ArrowLeft,
        protocol::KRIGHT => NamedKey::ArrowRight,
        protocol::KHOME => NamedKey::Home,
        protocol::KEND => NamedKey::End,
        protocol::KPGUP => NamedKey::PageUp,
        protocol::KPGDOWN => NamedKey::PageDown,
        protocol::KINS => NamedKey::Insert,
        protocol::KALT => NamedKey::Alt,
        protocol::KSHIFT => NamedKey::Shift,
        protocol::KCTL => NamedKey::Control,
        protocol::KF1..=protocol::KF12 => {
            const F_KEYS: [NamedKey; 12] = [
                NamedKey::F1,
                NamedKey::F2,
                NamedKey::F3,
                NamedKey::F4,
                NamedKey::F5,
                NamedKey::F6,
                NamedKey::F7,
                NamedKey::F8,
                NamedKey::F9,
                NamedKey::F10,
                NamedKey::F11,
                NamedKey::F12,
            ];
            F_KEYS[(rune - protocol::KF1) as usize]
        },
        _ => return None,
    })
}

fn modifiers_from_mask(mask: u16) -> Modifiers {
    let mut state = ModifiersState::empty();
    // gl9win2 does not report which side was pressed; report the left one.
    let mut pressed_mods = ModifiersKeys::empty();

    if mask & protocol::MOD_SHIFT != 0 {
        state |= ModifiersState::SHIFT;
        pressed_mods |= ModifiersKeys::LSHIFT;
    }
    if mask & protocol::MOD_CTRL != 0 {
        state |= ModifiersState::CONTROL;
        pressed_mods |= ModifiersKeys::LCONTROL;
    }
    if mask & protocol::MOD_ALT != 0 {
        state |= ModifiersState::ALT;
        pressed_mods |= ModifiersKeys::LALT;
    }
    if mask & protocol::MOD_SUPER != 0 {
        state |= ModifiersState::SUPER;
        pressed_mods |= ModifiersKeys::LSUPER;
    }

    Modifiers { state, pressed_mods }
}

fn element_state(pressed: bool) -> event::ElementState {
    if pressed {
        event::ElementState::Pressed
    } else {
        event::ElementState::Released
    }
}

#[derive(Default)]
struct EventState {
    /// Last modifier mask seen in a key record, to emit ModifiersChanged.
    modifiers_mask: u16,
}

pub struct EventLoop<T> {
    receiver: mpsc::Receiver<Message>,
    user_sender: mpsc::Sender<T>,
    user_receiver: mpsc::Receiver<T>,
    window_target: event_loop::ActiveEventLoop,
}

impl<T: 'static> EventLoop<T> {
    pub(crate) fn new(_: &PlatformSpecificEventLoopAttributes) -> Result<Self, EventLoopError> {
        let (sender, receiver) = mpsc::channel();
        let (user_sender, user_receiver) = mpsc::channel();

        // Blocking reader thread: stdin (fd 0) carries the gl9win2 event
        // records. There is no poll/select on Plan 9; this is the idiom.
        let reader_sender = sender.clone();
        std::thread::Builder::new()
            .name("winit stdin reader".into())
            .spawn(move || {
                let mut stdin = std::io::stdin();
                let mut buf = [0u8; protocol::RECORD_SIZE];
                loop {
                    match stdin.read_exact(&mut buf) {
                        Ok(()) => {
                            if reader_sender.send(Message::Record(Record::parse(&buf))).is_err() {
                                break;
                            }
                        },
                        Err(_) => {
                            // EOF without a quit record (gl9win2 died);
                            // synthesize one so the app can shut down.
                            let _ = reader_sender.send(Message::Record(Record {
                                typ: protocol::EV_QUIT,
                                state: 0,
                                modifiers: 0,
                                a: 0,
                                b: 0,
                            }));
                            break;
                        },
                    }
                }
            })
            .map_err(|error| EventLoopError::Os(os_error!(OsError::new(error))))?;

        Ok(Self {
            receiver,
            user_sender,
            user_receiver,
            window_target: event_loop::ActiveEventLoop {
                p: ActiveEventLoop {
                    control_flow: Cell::new(ControlFlow::default()),
                    exit: Cell::new(false),
                    creates: Mutex::new(VecDeque::new()),
                    sender,
                },
                _marker: PhantomData,
            },
        })
    }

    fn dispatch_record<F>(
        record: Record,
        event_state: &mut EventState,
        redraws: &mut VecDeque<WindowId>,
        mut event_handler: F,
    ) where
        F: FnMut(event::Event<T>),
    {
        let window_id = WindowId::PRIMARY;

        match record.typ {
            protocol::EV_KEY => {
                let pressed = record.state == 1;
                let rune = record.a;
                let named_key_opt = rune_to_named(rune);

                // Default to unidentified key with no text.
                let mut logical_key = Key::Unidentified(NativeKey::Unidentified);
                let mut key_without_modifiers = logical_key.clone();
                let mut text = None;
                let mut text_with_all_modifiers = None;

                // Printable keys carry their rune (with Shift already applied
                // by the Plan 9 keyboard driver, but not Ctrl).
                if let Some(character) = char::from_u32(rune).filter(|c| *c != '\0') {
                    let mut tmp = [0u8; 4];
                    let character_str: &str = character.encode_utf8(&mut tmp);
                    logical_key = Key::Character(character_str.into());
                    key_without_modifiers =
                        Key::Character(SmolStr::from_iter(character.to_lowercase()));
                    if pressed {
                        text = Some(SmolStr::new(character_str));
                        // The key with Ctrl applied as well (orbital-style).
                        let character_all_modifiers = if record.modifiers & protocol::MOD_CTRL != 0
                            && character.is_ascii_lowercase()
                        {
                            ((character as u8 - b'a') + 1) as char
                        } else {
                            character
                        };
                        text_with_all_modifiers =
                            Some(character_all_modifiers.encode_utf8(&mut tmp).into());
                    }
                }

                // Override key if a named key was found (e.g. Enter over '\n').
                if let Some(named_key) = named_key_opt {
                    logical_key = Key::Named(named_key);
                    key_without_modifiers = logical_key.clone();
                }

                event_handler(event::Event::WindowEvent {
                    window_id: RootWindowId(window_id),
                    event: event::WindowEvent::KeyboardInput {
                        device_id: event::DeviceId(DeviceId),
                        event: event::KeyEvent {
                            logical_key,
                            // No scancodes on the wire; alacritty binds on logical keys.
                            physical_key: PhysicalKey::Unidentified(NativeKeyCode::Unidentified),
                            location: KeyLocation::Standard,
                            state: element_state(pressed),
                            repeat: false,
                            text,
                            platform_specific: KeyEventExtra {
                                key_without_modifiers,
                                text_with_all_modifiers,
                            },
                        },
                        is_synthetic: false,
                    },
                });

                // gl9win2 tracks the modifier mask for us; diff it.
                if record.modifiers != event_state.modifiers_mask {
                    event_state.modifiers_mask = record.modifiers;
                    event_handler(event::Event::WindowEvent {
                        window_id: RootWindowId(window_id),
                        event: event::WindowEvent::ModifiersChanged(modifiers_from_mask(
                            record.modifiers,
                        )),
                    });
                }
            },
            protocol::EV_MOUSE_MOVE => {
                event_handler(event::Event::WindowEvent {
                    window_id: RootWindowId(window_id),
                    event: event::WindowEvent::CursorMoved {
                        device_id: event::DeviceId(DeviceId),
                        position: (record.a as i32, record.b as i32).into(),
                    },
                });
            },
            protocol::EV_MOUSE_BUTTON => {
                let button = match record.a {
                    protocol::BTN_LEFT => event::MouseButton::Left,
                    protocol::BTN_MIDDLE => event::MouseButton::Middle,
                    protocol::BTN_RIGHT => event::MouseButton::Right,
                    other => {
                        tracing::warn!("unhandled mouse button: {}", other);
                        return;
                    },
                };
                event_handler(event::Event::WindowEvent {
                    window_id: RootWindowId(window_id),
                    event: event::WindowEvent::MouseInput {
                        device_id: event::DeviceId(DeviceId),
                        state: element_state(record.state == 1),
                        button,
                    },
                });
            },
            protocol::EV_SCROLL => {
                event_handler(event::Event::WindowEvent {
                    window_id: RootWindowId(window_id),
                    event: event::WindowEvent::MouseWheel {
                        device_id: event::DeviceId(DeviceId),
                        delta: event::MouseScrollDelta::LineDelta(0.0, record.a as i32 as f32),
                        phase: event::TouchPhase::Moved,
                    },
                });
            },
            protocol::EV_RESIZE => {
                super::set_window_size(record.a, record.b);
                event_handler(event::Event::WindowEvent {
                    window_id: RootWindowId(window_id),
                    event: event::WindowEvent::Resized((record.a, record.b).into()),
                });

                // Require redraw after resize.
                if !redraws.contains(&window_id) {
                    redraws.push_back(window_id);
                }
            },
            protocol::EV_FOCUS => {
                event_handler(event::Event::WindowEvent {
                    window_id: RootWindowId(window_id),
                    event: event::WindowEvent::Focused(record.state == 1),
                });
            },
            protocol::EV_QUIT => {
                event_handler(event::Event::WindowEvent {
                    window_id: RootWindowId(window_id),
                    event: event::WindowEvent::CloseRequested,
                });
            },
            other => {
                tracing::warn!("unhandled event record type: {}", other);
            },
        }
    }

    pub fn run<F>(self, mut event_handler_inner: F) -> Result<(), EventLoopError>
    where
        F: FnMut(event::Event<T>, &event_loop::ActiveEventLoop),
    {
        let mut event_handler =
            move |event: event::Event<T>, window_target: &event_loop::ActiveEventLoop| {
                event_handler_inner(event, window_target);
            };

        let mut start_cause = StartCause::Init;
        let mut pending: VecDeque<Message> = VecDeque::new();
        let mut event_state = EventState::default();

        loop {
            event_handler(event::Event::NewEvents(start_cause), &self.window_target);

            if start_cause == StartCause::Init {
                event_handler(event::Event::Resumed, &self.window_target);
            }

            // Handle window creates: emit the initial size and position,
            // orbital-style. The real size follows as a normal Resized event
            // (gl9win2 guarantees a resize record before anything else).
            while let Some(window_id) = {
                let mut creates = self.window_target.p.creates.lock().unwrap();
                creates.pop_front()
            } {
                event_handler(
                    event::Event::WindowEvent {
                        window_id: RootWindowId(window_id),
                        event: event::WindowEvent::Resized(super::window_size()),
                    },
                    &self.window_target,
                );
                event_handler(
                    event::Event::WindowEvent {
                        window_id: RootWindowId(window_id),
                        event: event::WindowEvent::Moved((0, 0).into()),
                    },
                    &self.window_target,
                );
            }

            // Drain everything that is already waiting.
            let mut redraws: VecDeque<WindowId> = VecDeque::new();
            loop {
                let message = match pending.pop_front() {
                    Some(message) => message,
                    None => match self.receiver.try_recv() {
                        Ok(message) => message,
                        Err(_) => break,
                    },
                };

                match message {
                    Message::Record(record) => Self::dispatch_record(
                        record,
                        &mut event_state,
                        &mut redraws,
                        |event| event_handler(event, &self.window_target),
                    ),
                    Message::UserWake => {
                        while let Ok(user_event) = self.user_receiver.try_recv() {
                            event_handler(
                                event::Event::UserEvent(user_event),
                                &self.window_target,
                            );
                        }
                    },
                    Message::Redraw(window_id) => {
                        if !redraws.contains(&window_id) {
                            redraws.push_back(window_id);
                        }
                    },
                    Message::Destroyed(window_id) => {
                        event_handler(
                            event::Event::WindowEvent {
                                window_id: RootWindowId(window_id),
                                event: event::WindowEvent::Destroyed,
                            },
                            &self.window_target,
                        );
                    },
                }
            }

            for window_id in redraws.drain(..) {
                event_handler(
                    event::Event::WindowEvent {
                        window_id: RootWindowId(window_id),
                        event: event::WindowEvent::RedrawRequested,
                    },
                    &self.window_target,
                );
            }

            event_handler(event::Event::AboutToWait, &self.window_target);

            if self.window_target.p.exiting() {
                break;
            }

            // Wait for the next message per the requested control flow. The
            // ActiveEventLoop holds a sender, so the channel never disconnects.
            match self.window_target.p.control_flow() {
                ControlFlow::Poll => {
                    start_cause = StartCause::Poll;
                },
                ControlFlow::Wait => {
                    let start = Instant::now();
                    match self.receiver.recv() {
                        Ok(message) => pending.push_back(message),
                        Err(_) => break,
                    }
                    start_cause = StartCause::WaitCancelled { start, requested_resume: None };
                },
                ControlFlow::WaitUntil(requested_resume) => {
                    let start = Instant::now();
                    let timeout = requested_resume.saturating_duration_since(start);
                    match self.receiver.recv_timeout(timeout) {
                        Ok(message) => {
                            pending.push_back(message);
                            start_cause = StartCause::WaitCancelled {
                                start,
                                requested_resume: Some(requested_resume),
                            };
                        },
                        Err(mpsc::RecvTimeoutError::Timeout) => {
                            start_cause =
                                StartCause::ResumeTimeReached { start, requested_resume };
                        },
                        Err(mpsc::RecvTimeoutError::Disconnected) => break,
                    }
                },
            }
        }

        event_handler(event::Event::LoopExiting, &self.window_target);

        Ok(())
    }

    pub fn window_target(&self) -> &event_loop::ActiveEventLoop {
        &self.window_target
    }

    pub fn create_proxy(&self) -> EventLoopProxy<T> {
        EventLoopProxy {
            user_sender: self.user_sender.clone(),
            sender: self.window_target.p.sender.clone(),
        }
    }
}

pub struct EventLoopProxy<T: 'static> {
    user_sender: mpsc::Sender<T>,
    sender: mpsc::Sender<Message>,
}

impl<T> EventLoopProxy<T> {
    pub fn send_event(&self, event: T) -> Result<(), event_loop::EventLoopClosed<T>> {
        self.user_sender.send(event).map_err(|mpsc::SendError(x)| event_loop::EventLoopClosed(x))?;

        let _ = self.sender.send(Message::UserWake);

        Ok(())
    }
}

impl<T> Clone for EventLoopProxy<T> {
    fn clone(&self) -> Self {
        Self { user_sender: self.user_sender.clone(), sender: self.sender.clone() }
    }
}

impl<T> Unpin for EventLoopProxy<T> {}

pub struct ActiveEventLoop {
    control_flow: Cell<ControlFlow>,
    exit: Cell<bool>,
    pub(super) creates: Mutex<VecDeque<WindowId>>,
    pub(super) sender: mpsc::Sender<Message>,
}

impl ActiveEventLoop {
    pub fn create_custom_cursor(&self, source: CustomCursorSource) -> RootCustomCursor {
        let _ = source.inner;
        RootCustomCursor { inner: super::PlatformCustomCursor }
    }

    pub fn primary_monitor(&self) -> Option<MonitorHandle> {
        Some(MonitorHandle)
    }

    pub fn available_monitors(&self) -> VecDeque<MonitorHandle> {
        let mut v = VecDeque::with_capacity(1);
        v.push_back(MonitorHandle);
        v
    }

    #[inline]
    pub fn listen_device_events(&self, _allowed: DeviceEvents) {}

    #[inline]
    pub fn system_theme(&self) -> Option<Theme> {
        None
    }

    // ponytail: nothing on plan9 inspects handles; borrow the Orbital variant.
    #[cfg(feature = "rwh_05")]
    #[inline]
    pub fn raw_display_handle_rwh_05(&self) -> rwh_05::RawDisplayHandle {
        rwh_05::RawDisplayHandle::Orbital(rwh_05::OrbitalDisplayHandle::empty())
    }

    #[cfg(feature = "rwh_06")]
    #[inline]
    pub fn raw_display_handle_rwh_06(
        &self,
    ) -> Result<rwh_06::RawDisplayHandle, rwh_06::HandleError> {
        Ok(rwh_06::RawDisplayHandle::Orbital(rwh_06::OrbitalDisplayHandle::new()))
    }

    pub fn set_control_flow(&self, control_flow: ControlFlow) {
        self.control_flow.set(control_flow)
    }

    pub fn control_flow(&self) -> ControlFlow {
        self.control_flow.get()
    }

    pub(crate) fn exit(&self) {
        self.exit.set(true);
    }

    pub(crate) fn exiting(&self) -> bool {
        self.exit.get()
    }

    pub(crate) fn owned_display_handle(&self) -> OwnedDisplayHandle {
        OwnedDisplayHandle
    }
}

#[derive(Clone)]
pub(crate) struct OwnedDisplayHandle;

impl OwnedDisplayHandle {
    #[cfg(feature = "rwh_05")]
    #[inline]
    pub fn raw_display_handle_rwh_05(&self) -> rwh_05::RawDisplayHandle {
        rwh_05::OrbitalDisplayHandle::empty().into()
    }

    #[cfg(feature = "rwh_06")]
    #[inline]
    pub fn raw_display_handle_rwh_06(
        &self,
    ) -> Result<rwh_06::RawDisplayHandle, rwh_06::HandleError> {
        Ok(rwh_06::OrbitalDisplayHandle::new().into())
    }
}
