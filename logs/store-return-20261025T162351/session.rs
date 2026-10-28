//! One client connection: framing state, opcode dispatch and the scripted match flow (fakeserver.handle()).

use std::sync::Arc;
use std::time::Duration;

use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::TcpStream;
use tokio::sync::mpsc;

use crate::maps::BattleMap;
use crate::packets::{self as pk, HEADER_TYPE};
use crate::proto::{self, SeqState};
use crate::hub::{self, Hub, Member};

/// Server settings; env names match tools/fakeserver.py so the same test recipes work.
#[derive(Clone, Debug)]
pub struct Config {
    pub room_type: u8,
    pub map_index: u32,
    /// Match start packet: 501 (everybody on start point 0) or 117 (start point per slot).
    pub start_op: u16,
    /// Round time in s, counted from 191/146: the HUD clock is 120 s for normal maps (ctx+490518 default; 190/191/146 times do not
    /// change it). When it runs out the server ends the match (0 = never).
    pub round_time: f64,
    /// SPAWN_SAME=1: every slot on start point 0 (experiments: players next to each other)
    pub spawn_same: bool,
    /// TEAM_SPLIT=1: team = slot for everybody at match start (experiments: enemies without clicking the team colour)
    pub team_split: bool,
    pub auto_start: f64,
    pub turn_start: f64,
    pub turn_mode: u8,
    pub go_start: f64,
    pub begin_start: f64,
    pub begin_op: u16,
    pub play_start: f64,
    /// >0: send 191 (round begin) N s after 189 mode 0 — starts the clock of context 0, the one the match tick reads.
    pub post_play_start: f64,
    /// >0: answer C->S 111 (bomb) with 152 (id) and explode it with 151 after this many seconds; 0 = ignore bombs.
    pub bomb_fuse: f64,
    /// >0: N s after C->S 120 subtype 13 (death) send 146 (round result -> next round).
    pub death_end: f64,
    /// >0: N s after C->S 120 subtype 13 (death) send 173 (match over -> waiting room). Takes precedence over death_end.
    pub match_end: f64,
    /// Number of clients in the room before the server starts the match (AUTO_START seconds after the last one entered).
    pub players: usize,
    /// The 3 ports sent in opcode 15 next to the host (UDP endpoints of the server? env UDP_PORTS="a,b,c").
    pub udp_ports: [u16; 3],
    /// Items placed on the map before each match (S->C 553 list 2): "x,y,kind;..." from env ITEMS.
    pub items: Vec<(u8, u8, u32)>,
    /// Items lying on the floor (553 list 1): env FLOOR_ITEMS.
    pub floor_items: Vec<(u8, u8, u32)>,
    /// Address handed to the client in opcodes 7/15 (must be reachable from the client; 127.0.0.1 for Wine).
    pub server_ip: [u8; 4],
    pub maps: Vec<BattleMap>,
}

impl Config {
    pub fn from_env(maps: Vec<BattleMap>) -> Self {
        fn env<T: std::str::FromStr>(k: &str, d: T) -> T {
            std::env::var(k).ok().and_then(|v| v.parse().ok()).unwrap_or(d)
        }
        Config {
            room_type: env("ROOM_TYPE", 19),
            map_index: env("MAP_INDEX", 0),
            start_op: env("START_OP", 117),
            round_time: env("ROUND_TIME", 120.0),
            spawn_same: env("SPAWN_SAME", 0.0) > 0.0,
            team_split: env("TEAM_SPLIT", 0.0) > 0.0,
            auto_start: env("AUTO_START", 0.0),
            turn_start: env("TURN_START", 0.0),
            turn_mode: env("TURN_MODE", 4),
            go_start: env("GO_START", 0.0),
            begin_start: env("BEGIN_START", 0.0),
            begin_op: env("BEGIN_OP", 191),
            play_start: env("PLAY_START", 0.0),
            post_play_start: env("POST_PLAY_START", 0.0),
            bomb_fuse: env("BOMB_FUSE", 2.5),
            death_end: env("DEATH_END", 0.0),
            match_end: env("MATCH_END", 3.0),
            players: env("PLAYERS", 1usize),
            udp_ports: {
                let v: Vec<u16> = std::env::var("UDP_PORTS").unwrap_or_default().split(',').filter_map(|x| x.trim().parse().ok()).collect();
                [v.first().copied().unwrap_or(0), v.get(1).copied().unwrap_or(0), v.get(2).copied().unwrap_or(0)]
            },
            floor_items: parse_items("FLOOR_ITEMS"),
            items: std::env::var("ITEMS")
                .unwrap_or_default()
                .split(';')
                .filter_map(|e| {
                    let v: Vec<u32> = e.split(',').filter_map(|x| x.trim().parse().ok()).collect();
                    (v.len() == 3).then(|| (v[0] as u8, v[1] as u8, v[2]))
                })
                .collect(),
            server_ip: env::<std::net::Ipv4Addr>("SERVER_IP", std::net::Ipv4Addr::LOCALHOST).octets(),
            maps,
        }
    }
}

pub type Log = Arc<dyn Fn(String) + Send + Sync>;

/// Writer side: owns the client's receive sequence (seq_r) so frames go out in order.
async fn writer_task(mut sock: tokio::net::tcp::OwnedWriteHalf, mut rx: mpsc::UnboundedReceiver<Vec<u8>>, log: Log, port: u16) {
    let mut st = SeqState { header_type: HEADER_TYPE, ..Default::default() };
    st.seq_r += st.dseq_r; // the hello consumed one receive step on the client
    while let Some(body) = rx.recv().await {
        let frame = proto::build(&body, 3, 2, st.recv_seq_byte(), st.seq_r, st.header_type);
        st.seq_r = st.seq_r.wrapping_add(st.dseq_r);
        log(format!("[{port}] S->C opcode {}", proto::get_int(&body, 2, true)));
        if sock.write_all(&frame).await.is_err() {
            break;
        }
    }
}

pub async fn handle(sock: TcpStream, port: u16, cfg: Arc<Config>, log: Log, hub: Arc<Hub>) {
    let peer = sock.peer_addr().map(|a| a.to_string()).unwrap_or_default();
    log(format!("[{port}] connect from {peer}"));
    let (mut rd, mut wr) = sock.into_split();
    if wr.write_all(&pk::hello_packet(HEADER_TYPE, pk::CLIENT_VERSION)).await.is_err() {
        return;
    }
    let (tx, rx) = mpsc::unbounded_channel();
    tokio::spawn(writer_task(wr, rx, log.clone(), port));

    let mut st = SeqState { header_type: HEADER_TYPE, ..Default::default() };
    let mut buf = Vec::new();
    let mut user_id: u32 = 0; // account of this connection (from opcode 1, or found in opcode 6 on the channel connection)
    let mut in_room = false;
    // 83 sent (create/join): the next 337 is the waiting room asking for its players. 337 also comes from the lobby after 23
    // (sub_8C0D23 -> sub_8BC5ED, u32 of dword_E60814) and must not put the user in the room
    let mut room_pending = false;
    let mut in_lobby = false;
    // the first C->S 88 after entering a room is automatic (the client's own list position), not a choice
    let mut map_synced = false;
    // shop wallet of this connection (not persisted): cash = XU (221), gold = lucci / VANG (20)
    let mut cash = pk::SHOP_CASH;
    let mut gold = pk::SHOP_GOLD;
    let mut chunk = vec![0u8; 65536];
    loop {
        let n = match tokio::time::timeout(Duration::from_secs(120), rd.read(&mut chunk)).await {
            Ok(Ok(n)) if n > 0 => n,
            Ok(_) => break,
            Err(_) => {
                log(format!("[{port}] idle timeout"));
                break;
            }
        };
        buf.extend_from_slice(&chunk[..n]);
        while let Some(len) = proto::frame_len(&buf, 2, 2) {
            let frame: Vec<u8> = buf.drain(..len).collect();
            let body = match proto::parse(&frame, 2, 2, st.seq_s, st.header_type) {
                Ok(b) => b,
                Err(_) => {
                    log(format!("[{port}] checksum mismatch"));
                    continue;
                }
            };
            st.seq_s = st.seq_s.wrapping_add(st.dseq_s);
            let op = proto::get_int(&body, 2, true) as u16;
            log(format!("[{port}] C->S opcode {op} body {}", hex(&body)));
            if op == 1 {
                user_id = hub.new_user();
                log(format!("[{port}] login -> user {user_id}"));
                let _ = tx.send(pk::login_ok_as(&hub::player_name(user_id), user_id, 1));
                let _ = tx.send(pk::login_net_info(&std::net::Ipv4Addr::from(cfg.server_ip).to_string(), cfg.udp_ports));
                // Vietnamese UI texts (the VN client has none for the UI keys)
                let _ = tx.send(pk::client_strings(crate::strings::VI));
            } else if op == 6 {
                // channel login on a new connection: find which account it is (u32 user id somewhere in the body)
                user_id = find_user(&body, hub.known_users()).unwrap_or(1000);
                log(format!("[{port}] channel login user {user_id}"));
                let _ = tx.send(pk::channel_enter_ok(now_utc()));
            } else if op == 395 {
                // lobby entry: same replies as before but with the real room list
                hub.lobby.lock().unwrap().push(tx.clone());
                {
                    let mut users = hub.lobby_users.lock().unwrap();
                    if !users.contains(&user_id) {
                        users.push(user_id);
                    }
                }
                in_lobby = true;
                hub.push_user_list();
                let _ = tx.send(pk::account_info(&hub::player_name(user_id)));
                let _ = tx.send(pk::item_catalog());
                // everyone in the lobby on the square map (slot = order of arrival), ourselves included
                let users: Vec<(u16, u32, String, u16, u16)> = hub.lobby_users.lock().unwrap().iter().enumerate()
                    .map(|(i, &u)| (i as u16, u, hub::player_name(u), SQUARE_X + 40 * i as u16, SQUARE_Y)).collect();
                for r in replies(op, port, &cfg) {
                    let _ = tx.send(match proto::get_int(&r, 2, true) {
                        88 => hub.room_list(cfg.room_type),
                        467 => pk::square_user_list(&users),
                        _ => r,
                    });
                }
            } else if op == 78 {
                // join a room from the lobby list (sub_4CB829: u16 session, u32 room id, str password, ...) -> same answer as create
                log(format!("[{port}] join room request from user {user_id}"));
                room_pending = true;
                let _ = tx.send(pk::create_room_ok(1, cfg.room_type, 0));
            } else if op == 88 && body.len() >= 8 {
                // map choice in the waiting room (sub_6C3557): u16 CPlayMapInfoList index (sub_6DC06C), u32 231-table index.
                // The client locks its buttons (vf153(0), state 1) until 113 arrives (CPrepareStage__On_113 -> vf153(1)).
                let code = proto::get_int(&body[2..], 2, true) as u16;
                let index = proto::get_int(&body[4..], 4, true) as u32;
                let ok = (index as usize) < cfg.maps.len();
                let auto = !map_synced;
                map_synced = true;
                log(format!("[{port}] map choice from user {user_id}: index {index} code {code}{}", if auto { " (automatic on entry: room map kept)" } else if ok { "" } else { " (unknown, ignored)" }));
                if ok && !auto {
                    hub.room.lock().unwrap().map = Some((index, code));
                    hub.broadcast(&pk::room_map(index, code));
                } else {
                    let (i, c) = room_map_of(&hub, &cfg);
                    let _ = tx.send(pk::room_map(i, c));
                }
            } else if op == 94 && body.len() >= 4 {
                // open/close an empty slot (sub_6BE8AF, room master): u8 slot, u8 closed. The client waits (state 2) for the 106 of
                // that slot; bit 0 of its flags = closed (On_106 0x6C6BC9). A taken slot is left alone.
                let slot = proto::get_int(&body[2..], 1, true) as u8;
                let flag = proto::get_int(&body[3..], 1, true) != 0;
                let owner = hub.room.lock().unwrap().members.iter().find(|m| m.slot == slot).map(|m| m.user_id);
                if let Some(owner) = owner {
                    log(format!("[{port}] slot {slot} closed={flag}: taken by user {owner}, ignored"));
                    hub.update_member(owner, |_| {});
                } else {
                    log(format!("[{port}] slot {slot} closed={flag}"));
                    if slot < 16 {
                        let mut r = hub.room.lock().unwrap();
                        if flag { r.closed |= 1 << slot } else { r.closed &= !(1 << slot) }
                    }
                    hub.broadcast(&pk::slot_closed(slot, flag));
                }
            } else if op == 89 && body.len() >= 3 {
                // team choice (sub_6BE71D): u8 = local player << 4 | team (5 bits in 106)
                let v = proto::get_int(&body[2..], 1, true) as u8;
                log(format!("[{port}] team {} (local player {})", v & 0xF, v >> 4));
                hub.update_member(user_id, |m| m.team = v & 0xF);
            } else if op == 90 && body.len() >= 4 {
                // character choice (sub_6BE7D0): u8 local player, u8 character (6 bits in 106)
                let ch = proto::get_int(&body[3..], 1, true) as u8;
                log(format!("[{port}] character {ch}"));
                hub.update_member(user_id, |m| m.character = ch & 0x3F);
            } else if op == 18 && body.len() >= 4 {
                // chat (sub_6709C2 <- CChatStage sub_68929F): u8 flag, u8 n, n x str lines. Echo to the room (or the lobby) as 28.
                let n = proto::get_int(&body[3..], 1, true) as usize;
                let mut pos = 4;
                let mut lines = Vec::new();
                for _ in 0..n.min(8) {
                    if pos + 2 > body.len() || pos + 2 + proto::get_int(&body[pos..], 2, true) as usize > body.len() {
                        break;
                    }
                    let (raw, used) = proto::get_str(&body[pos..], true);
                    lines.push(proto::from_cp1258(&raw));
                    pos += used;
                }
                let name = hub::player_name(user_id);
                log(format!("[{port}] chat {name}: {lines:?}"));
                for l in lines {
                    let pkt = pk::chat_line(&format!("{name} : {l}"));
                    if in_room {
                        hub.broadcast(&pkt);
                    } else {
                        let mut lobby = hub.lobby.lock().unwrap();
                        lobby.retain(|t| !t.is_closed());
                        for t in lobby.iter() {
                            let _ = t.send(pkt.clone());
                        }
                    }
                }
            } else if op == 325 {
                // SHOP button (stage vf65 -> sub_7EFA8E, after 23 set the account flag): u8. 289 {u8 1} -> sub_7EFC2E -> shop stage
                log(format!("[{port}] shop request -> 289"));
                let _ = tx.send(pk::shop_enter_ok());
            } else if op == 38 {
                // after 289: enter-shop request (sub_7EFB6A): u8, u8 n, n x (str name, u32 id), u8 -> 20 (CStage__On_20, stage 14)
                log(format!("[{port}] shop enter 38 -> 20"));
                let _ = tx.send(pk::shop_data(gold));
            } else if op == 102 && body.len() >= 3 && proto::get_int(&body[2..], 1, true) == 0 {
                // CShopStageNew::vf80 waits for On_100 (u16 destination stage) before leaving the shop.
                let stage = if in_room { 12 } else { 11 };
                log(format!("[{port}] shop leave 102 -> 100 (stage {stage})"));
                let _ = tx.send(pk::Packet::new(100).u16(stage).build());
            } else if op == 260 || op == 259 {
                // cash requests: 260 on shop entry (sub_800DF0), 259 after a purchase; bool -> 221 {u8 slot, u32 cash}
                log(format!("[{port}] cash {op} -> 221 ({cash})"));
                let _ = tx.send(pk::cash_balance(cash));
            } else if op == 261 && body.len() >= 7 {
                // buy (sub_8AF502 -> sub_8A9833): u32 stock id, u8 currency, 4 x u8, after the client-side money/level checks.
                // 223 {u8 0} = ok; the client deducts gold itself but asks for cash again with 259
                let stock = proto::get_int(&body[2..], 4, true) as u32;
                let code = match pk::SHOP_ITEMS.iter().find(|it| it.0 == stock) {
                    Some(&(_, _, _, _, _, price, 0)) if cash >= price => { cash -= price; 0 }
                    Some(&(_, _, _, _, _, price, 1)) if gold >= price => { gold -= price; 0 }
                    Some(_) => 1,
                    None => 41,
                };
                log(format!("[{port}] buy {stock} -> 223 {code} (cash {cash}, gold {gold})"));
                let _ = tx.send(pk::buy_result(code));
            } else if op == 64 && body.len() >= 4 {
                // lobby user list page (sub_679EF4): u16 page -> 69 (11 per page)
                let page = proto::get_int(&body[2..], 2, true) as u16;
                log(format!("[{port}] user list page {page} -> 69"));
                let _ = tx.send(hub.user_list_page(page));
            } else if op == 276 || op == 278 || op == 256 {
                // shop list requests after 20 when the cached .ini version differs: FxStock -> 64, StringTable -> 225, PackageMap -> 65
                let pkt = match op { 276 => pk::shop_stock(), 278 => pk::shop_strings(), _ => pk::shop_packages() };
                log(format!("[{port}] shop list {op}"));
                let _ = tx.send(pkt);
            } else if op == 95 && body.len() >= 6 {
                // kick by the master (sub_6C6E36 "SureToBan"): u32 user id (+12 of the slot record), u8
                let target = proto::get_int(&body[2..], 4, true) as u32;
                let is_chief = hub.room.lock().unwrap().chief == Some(user_id);
                log(format!("[{port}] kick user {target} by {user_id} (master: {is_chief})"));
                if is_chief && target != user_id && hub.leave_room(target, true).is_some() {
                    hub.push_room_list(cfg.room_type);
                }
            } else if op == 81 && in_room {
                // back to the lobby from the room (sub_8F2F14)
                log(format!("[{port}] user {user_id} leaves the room"));
                in_room = false;
                if hub.leave_room(user_id, false).is_some() {
                    hub.push_room_list(cfg.room_type);
                }
            } else if op == 87 && body.len() >= 7 {
                // READY of a non-master (sub_6C31ED): blob (sub_8FD1D5, room/player info), bool ready, u32. Answer: 106 with bit 0.
                let ready = proto::get_int(&body[body.len() - 5..], 1, true) != 0;
                log(format!("[{port}] ready={ready} from user {user_id}"));
                hub.update_member(user_id, |m| m.ready = ready);
            } else if op == 97 {
                // START by the master (sub_6C869F, after its own all-ready check) -> server-driven match start
                let start = {
                    let mut r = hub.room.lock().unwrap();
                    let ok = !r.started && !r.members.is_empty() && r.chief == Some(user_id);
                    if ok {
                        r.started = true;
                        for m in r.members.iter_mut() {
                            m.ready = false;
                        }
                    }
                    ok
                };
                log(format!("[{port}] start request from user {user_id} -> {}", if start { "match" } else { "ignored" }));
                if start {
                    tokio::spawn(match_flow(hub.clone(), cfg.clone(), log.clone(), port, 1.0));
                }
            } else if op == 337 && !room_pending && !hub.room.lock().unwrap().members.iter().any(|m| m.user_id == user_id) {
                log(format!("[{port}] 337 outside a room join (data request), ignored"));
            } else if op == 337 {
                room_pending = false;
                in_room = true;
                hub.lobby.lock().unwrap().retain(|t| !t.same_channel(&tx));
                enter_room(&hub, &cfg, &log, port, user_id, &tx);
                map_synced = false;
                hub.push_room_list(cfg.room_type);
            } else {
                if op == 77 {
                    room_pending = true; // create room -> 83 -> the waiting room sends 337
                }
                for r in replies(op, port, &cfg) {
                    let _ = tx.send(r);
                }
            }
            if op == 117 {
                if let Some(r) = pk::parse_item_pick(&body) {
                    log(format!("[{port}] item pick {r:?} -> 158"));
                    hub.broadcast(&pk::item_picked(&r));
                }
            }
            if op == 111 && cfg.bomb_fuse > 0.0 {
                if let Some(rec) = pk::parse_bomb_record(&body) {
                    let id = hub.next_bomb_id();
                    log(format!("[{port}] bomb {rec:?} -> 152 id {id}, 151 in {}s", cfg.bomb_fuse));
                    hub.broadcast(&pk::bomb_placed(&rec, id));
                    let (hub2, fuse) = (hub.clone(), cfg.bomb_fuse);
                    tokio::spawn(async move {
                        tokio::time::sleep(Duration::from_secs_f64(fuse)).await;
                        hub2.broadcast(&pk::bomb_explode(&[id], 2));
                    });
                }
            }
            if op == 92 && body.len() >= 12 {
                // C->S 92 (CGameStage vf161 0x7027B0, once at match start): u32 ip1, u32 ip2 (sub_9B5552), u16 UDP port (sub_9B555E), ...
                let ip1 = proto::get_int(&body[2..], 4, true) as u32;
                let ip2 = proto::get_int(&body[6..], 4, true) as u32;
                let uport = proto::get_int(&body[10..], 2, true) as u16;
                log(format!("[{port}] udp info user {user_id}: {:08x} {:08x} port {uport}", ip1, ip2));
                if let Some(m) = hub.room.lock().unwrap().members.iter_mut().find(|m| m.user_id == user_id) {
                    m.udp = Some((ip1, ip2, uport));
                }
            }
            if op == 120 && body.len() > 2 {
                // bomber event: u8 subtype (11 trapped, 13 died; oc_e-game.md section 5a-5c)
                let sub = proto::get_int(&body[2..], 1, true) as u8;
                log(format!("[{port}] bomber event 120 subtype {sub}"));
                // several players: the match ends when at most one team is left (one team only: all dead) -> 168 (survivors rank 1), 173
                let survivors = {
                    let mut r = hub.room.lock().unwrap();
                    if let Some(m) = r.members.iter_mut().find(|m| m.user_id == user_id) {
                        if sub == 13 {
                            m.dead = true;
                        }
                    }
                    let alive: Vec<&Member> = r.members.iter().filter(|m| !m.dead).collect();
                    let teams: std::collections::HashSet<u8> = alive.iter().map(|m| m.team).collect();
                    let all_teams: std::collections::HashSet<u8> = r.members.iter().map(|m| m.team).collect();
                    // everybody on one team (co-op): over only when all are dead
                    let over = if all_teams.len() > 1 { teams.len() <= 1 } else { alive.is_empty() };
                    (sub == 13 && r.members.len() > 1 && over)
                        .then(|| r.members.iter().map(|m| (m.slot, if m.dead { 2 } else { 1 })).collect::<Vec<_>>())
                };
                if let Some(ranks) = survivors {
                    let no = hub.room.lock().unwrap().match_no;
                    log(format!("[{port}] match decided: ranks {ranks:?} -> 168, 173 in {}s", cfg.match_end.max(3.0) + 5.0));
                    hub.finish_match(no, ranks, cfg.match_end.max(3.0) + 5.0);
                } else if sub == 13 && hub.room.lock().unwrap().members.len() <= 1 && (cfg.match_end > 0.0 || cfg.death_end > 0.0) {
                    let (hub2, cfg2) = (hub.clone(), cfg.clone());
                    tokio::spawn(async move {
                        if cfg2.match_end > 0.0 {
                            tokio::time::sleep(Duration::from_secs_f64(cfg2.match_end)).await;
                            hub2.room.lock().unwrap().started = false;
                            hub2.broadcast(&pk::match_over());
                        } else {
                            tokio::time::sleep(Duration::from_secs_f64(cfg2.death_end)).await;
                            hub2.broadcast(&pk::round_result(0, 10000, (180, 30), 0, (0, 0, 0), &[(0, [(1, 1), (0, 0), (0, 0)])]));
                        }
                    });
                }
            }
        }
    }
    if in_room && hub.leave_room(user_id, false).is_some() {
        hub.push_room_list(cfg.room_type);
    }
    if in_lobby {
        hub.lobby_users.lock().unwrap().retain(|&u| u != user_id);
        hub.push_user_list();
    }
    log(format!("[{port}] close {peer}"));
}

/// Spawn point on the square map (pixels).
const SQUARE_X: u16 = 600;
const SQUARE_Y: u16 = 400;

fn replies(op: u16, port: u16, cfg: &Config) -> Vec<Vec<u8>> {
    match op {
        0 => pk::login_replies(),
        1 => vec![pk::login_ok("tester", 1), pk::login_net_info(&std::net::Ipv4Addr::from(cfg.server_ip).to_string(), [0, 0, 0])],
        40 => vec![pk::channel_list(&["Kenh 1", "Kenh 2", "Kenh 3", "Kenh 4"], 100), pk::channel_status(&[10, 20, 30, 40])],
        5 => vec![pk::channel_join_ok(cfg.server_ip, port, 10, 0)],
        6 => vec![pk::channel_enter_ok(now_utc())],
        77 => vec![pk::create_room_ok(1, cfg.room_type, 0)],
        337 => vec![
            pk::room_player(0, 1000, "tester", true, 0, 0),
            pk::room_slot_state(0, true, false, 0, 0),
            pk::room_map(cfg.map_index, 0),
        ],
        395 => vec![
            pk::square_info(1, "LobbySquare01", "Square"), // "Map/%s" = pack entry, no ".map" (else grey square)
            pk::square_users(),
            pk::room_list(),
            pk::map_checksums(&cfg.maps),
            pk::dyn_map_info(&cfg.maps),
        ],
        _ => vec![],
    }
}

/// Server-driven match: 501 -> 189 (TURN_MODE) -> 190 -> 191/146 -> 189 mode 0 (docs/oc_e-game.md §5a-3).
async fn match_flow(hub: Arc<Hub>, cfg: Arc<Config>, log: Log, port: u16, delay: f64) {
    let wait = |s: f64| tokio::time::sleep(Duration::from_secs_f64(s));
    wait(delay).await;
    // the map is read after the wait: the master may still pick one (C->S 88) in the meantime
    let (map_index, _) = room_map_of(&hub, &cfg);
    let map = cfg.maps.get(map_index as usize).unwrap_or(&cfg.maps[0]);
    let match_no = {
        let mut r = hub.room.lock().unwrap();
        for m in r.members.iter_mut() {
            m.dead = false;
            if cfg.team_split {
                m.team = m.slot;
            }
        }
        r.match_no += 1;
        r.match_no
    };
    if !cfg.items.is_empty() || !cfg.floor_items.is_empty() {
        // item layout must arrive before 501 (read when the map loads); list 1 = floor, list 2 = hidden in bricks
        let bricks: Vec<(u16, u8, u8, u32)> = cfg.items.iter().enumerate().map(|(i, &(x, y, k))| (i as u16, x, y, k)).collect();
        let n = bricks.len();
        let floor: Vec<(u16, u8, u8, u32)> = cfg.floor_items.iter().enumerate().map(|(i, &(x, y, k))| ((n + i) as u16, x, y, k)).collect();
        hub.broadcast(&pk::map_items(&floor, &bricks));
    }
    let slots: Vec<(u8, u8, u8)> = hub.room.lock().unwrap().members.iter().map(|m| (m.slot, m.slot, m.team)).collect();
    if cfg.start_op == 117 {
        // start point i for slot i (501 puts everybody on start point 0). u8 +84 must stay 0: the character there made the
        // other player vanish from the match (logs/duo/t13)
        let full: Vec<(u8, u8, u8, u8, u8)> =
            hub.room.lock().unwrap().members.iter().map(|m| (m.slot, 0, if cfg.spawn_same { 0 } else { m.slot }, m.slot, m.team)).collect();
        // time byte 0: it only applies when the map record enables a custom time (sub_8E7E3F, set from sub_6DD49B(map))
        hub.broadcast(&pk::match_start_full(map, map_index, cfg.room_type, 12345, &full, 0));
    } else {
        hub.broadcast(&pk::match_start(map, map_index, cfg.room_type, 12345, &slots));
    }
    log(format!("[{port}] match flow: {} map {map_index}", cfg.start_op));
    if cfg.turn_start <= 0.0 {
        return;
    }
    wait(cfg.turn_start).await;
    if cfg.turn_mode == 1 {
        // start point per slot: slot i -> start point i (all on index 1 put everybody on the same cell)
        let mut spawns = [0u8; 16];
        for m in &hub.room.lock().unwrap().members {
            spawns[m.slot as usize] = m.slot;
        }
        hub.broadcast(&pk::round_load(map, 10000, &spawns));
    } else {
        hub.broadcast(&pk::round_start(map, cfg.turn_mode, 10000, (1, 1, 12345), (hub.slot_mask(), 1)));
    }
    if cfg.go_start <= 0.0 {
        return;
    }
    wait(cfg.go_start).await;
    hub.broadcast(&pk::round_go((if cfg.round_time > 0.0 { cfg.round_time as u32 } else { 120 }, 30), 0, 0, 0));
    if cfg.begin_start > 0.0 {
        wait(cfg.begin_start).await;
        let r = if cfg.begin_op == 552 {
            crate::packets::Packet::new(552).u32(0).build()
        } else if cfg.begin_op == 146 {
            pk::round_result(0, 1, (180, 30), 0, (0, 0, 0), &[])
        } else {
            pk::round_begin(0, 180_000, 30_000, 0)
        };
        hub.broadcast(&r);
    }
    if cfg.play_start > 0.0 {
        wait(cfg.play_start).await;
        hub.broadcast(&pk::round_start(map, 0, 10000, (1, 1, 12345), (1, 1)));
        if cfg.post_play_start > 0.0 {
            wait(cfg.post_play_start).await;
            let rt = if cfg.round_time > 0.0 { cfg.round_time as u32 } else { 180 };
            // 146 = round begin + UDP peer table (sub_70A647: per slot 3 x (u32 ip, u16 port) -> sub_9B5C5A -> MTUDP HostCreate);
            // fall back to 191 when some client did not report its UDP address (C->S 92)
            let peers: Option<Vec<(usize, [(u32, u16); 3])>> = hub
                .room
                .lock()
                .unwrap()
                .members
                .iter()
                .map(|m| m.udp.map(|(a, b, p)| (m.slot as usize, [(0x7F00_0001, p), (a, p), (b, p)])))
                .collect();
            match peers {
                Some(p) if cfg.players > 1 => {
                    log(format!("[{port}] 146 with UDP peers {p:x?}"));
                    hub.broadcast(&pk::round_result(0, 10000, (rt, 30), 0, (0, 0, 0), &p));
                }
                _ => hub.broadcast(&pk::round_begin(0, rt * 1000, 30_000, 0)),
            }
            if cfg.round_time > 0.0 {
                // time limit: the HUD clock counts down the time of 191/146 and stops at 00:00 without sending anything ->
                // the server ends the match: everybody still alive ranks 1
                let (hub2, log2, t) = (hub.clone(), log.clone(), cfg.round_time);
                tokio::spawn(async move {
                    tokio::time::sleep(Duration::from_secs_f64(t + 1.0)).await;
                    let ranks: Vec<(u8, u8)> =
                        hub2.room.lock().unwrap().members.iter().map(|m| (m.slot, if m.dead { 2 } else { 1 })).collect();
                    if hub2.finish_match(match_no, ranks.clone(), 8.0) {
                        log2(format!("[{port}] time up: ranks {ranks:?} -> 168, 173"));
                    }
                });
            }
        }
    }
}

/// UTC wall clock as [y, m, d, h, mi, s] (the client only displays it).
fn now_utc() -> [u16; 6] {
    let secs = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map_or(0, |d| d.as_secs());
    let (days, rem) = ((secs / 86400) as i64, secs % 86400);
    // civil_from_days (H. Hinnant)
    let z = days + 719_468;
    let era = z.div_euclid(146_097);
    let doe = z - era * 146_097;
    let yoe = (doe - doe / 1460 + doe / 36524 - doe / 146_096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = doy - (153 * mp + 2) / 5 + 1;
    let m = if mp < 10 { mp + 3 } else { mp - 9 };
    let y = yoe + era * 400 + (m <= 2) as i64;
    [y as u16, m as u16, d as u16, (rem / 3600) as u16, (rem % 3600 / 60) as u16, (rem % 60) as u16]
}

fn hex(b: &[u8]) -> String {
    b.iter().map(|x| format!("{x:02x}")).collect()
}

/// "x,y,kind;..." from an env var.
fn parse_items(var: &str) -> Vec<(u8, u8, u32)> {
    std::env::var(var)
        .unwrap_or_default()
        .split(';')
        .filter_map(|e| {
            let v: Vec<u32> = e.split(',').filter_map(|x| x.trim().parse().ok()).collect();
            (v.len() == 3).then(|| (v[0] as u8, v[1] as u8, v[2]))
        })
        .collect()
}

/// Find a known account id stored as an obfuscated u32 anywhere in a body (opcode 6 carries the id from the login).
fn find_user(body: &[u8], known: std::ops::Range<u32>) -> Option<u32> {
    (2..body.len().saturating_sub(3)).map(|i| proto::get_int(&body[i..], 4, true) as u32).find(|v| known.contains(v))
}

/// Opcode 337 (entering the waiting room): put the user into the shared room, tell everybody, start the match
/// when `PLAYERS` clients are in. Slot 0 = room chief.
/// The room's map: the last C->S 88 choice, else MAP_INDEX (code 0 = let the client look it up).
fn room_map_of(hub: &Hub, cfg: &Config) -> (u32, u16) {
    hub.room.lock().unwrap().map.unwrap_or((cfg.map_index, 0))
}

fn enter_room(hub: &Arc<Hub>, cfg: &Arc<Config>, log: &Log, port: u16, user_id: u32, tx: &mpsc::UnboundedSender<Vec<u8>>) {
    let (slot, others, start, closed, chief) = {
        let mut r = hub.room.lock().unwrap();
        r.members.retain(|m| m.user_id != user_id);
        let closed = r.closed;
        let slot = (0..16u8).find(|s| closed & (1 << s) == 0 && !r.members.iter().any(|m| m.slot == *s)).unwrap_or(0);
        let others: Vec<(u8, u32, String, bool, u8, u8)> =
            r.members.iter().map(|m| (m.slot, m.user_id, m.name.clone(), m.ready, m.character, m.team)).collect();
        if r.chief.is_none() || !r.members.iter().any(|m| Some(m.user_id) == r.chief) {
            r.chief = Some(user_id);
        }
        r.members.push(Member { user_id, name: hub::player_name(user_id), slot, tx: tx.clone(), udp: None, team: 0, character: 0, ready: false, dead: false });
        let start = !r.started && r.members.len() >= cfg.players && cfg.auto_start > 0.0;
        if start {
            r.started = true;
        }
        (slot, others, start, closed, r.chief)
    };
    let name = hub::player_name(user_id);
    log(format!("[{port}] user {user_id} enters the room in slot {slot} ({} others)", others.len()));
    // the newcomer: everybody already in the room, then itself
    for (s, id, nm, ready, ch, team) in &others {
        let _ = tx.send(pk::room_player(*s, *id, nm, chief == Some(*id), *ch, *team));
        let _ = tx.send(pk::room_slot_state(*s, chief == Some(*id), *ready, *ch, *team));
    }
    let me_chief = chief == Some(user_id);
    let _ = tx.send(pk::room_player(slot, user_id, &name, me_chief, 0, 0));
    let _ = tx.send(pk::room_slot_state(slot, me_chief, false, 0, 0));
    for s in (0..16u8).filter(|s| closed & (1 << s) != 0) {
        let _ = tx.send(pk::slot_closed(s, true));
    }
    let (map_index, map_code) = room_map_of(hub, cfg);
    let _ = tx.send(pk::room_map(map_index, map_code));
    // the others: the newcomer
    for m in hub.room.lock().unwrap().members.iter().filter(|m| m.user_id != user_id) {
        let _ = m.tx.send(pk::room_player(slot, user_id, &name, me_chief, 0, 0));
        let _ = m.tx.send(pk::room_slot_state(slot, me_chief, false, 0, 0));
    }
    if start {
        tokio::spawn(match_flow(hub.clone(), cfg.clone(), log.clone(), port, cfg.auto_start));
    }
}
