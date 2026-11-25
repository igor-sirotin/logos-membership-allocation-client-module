use std::fs;
use std::io::{ErrorKind, Write as _};
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex, MutexGuard};
use std::time::{Duration, Instant};

use serde_json::{json, Value};

const KEY_FILE: &str = "allocation_node.key";
const KEY_FILE_BEING_WRITTEN: &str = "allocation_node.key.tmp";

pub type Shim = Arc<dyn Fn(&str, &Value) -> Result<Value, String> + Send + Sync>;

struct Live {
    bootstrap_nodes: Vec<Value>,
    used: Instant,
}

pub struct Keeper {
    shim: Shim,
    key_dir: Option<PathBuf>,
    live: Option<Live>,
    calls_in_flight: usize,
}

fn lock(keeper: &Mutex<Keeper>) -> MutexGuard<'_, Keeper> {
    keeper.lock().unwrap_or_else(|e| e.into_inner())
}

fn is_key(text: &str) -> bool {
    hex::decode(text).is_ok_and(|bytes| !bytes.is_empty())
}

fn store_key(path: &Path, key: &str) -> Result<(), String> {
    let told = |e: std::io::Error| format!("{}: {e}", path.display());
    let dir = path.parent().unwrap_or(Path::new("."));
    fs::create_dir_all(dir).map_err(told)?;
    let being_written = dir.join(KEY_FILE_BEING_WRITTEN);
    let mut options = fs::OpenOptions::new();
    options.write(true).create(true).truncate(true);
    #[cfg(unix)]
    {
        use std::os::unix::fs::OpenOptionsExt as _;
        options.mode(0o600);
    }
    let mut file = options.open(&being_written).map_err(told)?;
    file.write_all(key.as_bytes()).map_err(told)?;
    file.sync_all().map_err(told)?;
    fs::rename(&being_written, path).map_err(told)
}

fn wanted_nodes(args: &Value) -> Result<Vec<Value>, String> {
    let config = match args.get(0) {
        Some(Value::String(text)) => {
            serde_json::from_str(text).map_err(|e| format!("createNode: config: {e}"))?
        }
        Some(other) => other.clone(),
        None => json!({}),
    };
    Ok(config
        .get("bootstrapNodes")
        .and_then(Value::as_array)
        .cloned()
        .unwrap_or_default())
}

impl Keeper {
    pub fn new(shim: Shim, key_dir: Option<PathBuf>) -> Self {
        Keeper {
            shim,
            key_dir,
            live: None,
            calls_in_flight: 0,
        }
    }

    fn key(&self) -> Result<Option<String>, String> {
        let Some(dir) = &self.key_dir else {
            return Ok(None);
        };
        let path = dir.join(KEY_FILE);
        match fs::read_to_string(&path) {
            Ok(text) if is_key(text.trim()) => return Ok(Some(text.trim().to_owned())),
            Ok(_) => return Err(format!("{}: not a hex key", path.display())),
            Err(e) if e.kind() == ErrorKind::NotFound => {}
            Err(e) => return Err(format!("{}: {e}", path.display())),
        }
        let key = (self.shim)("newPrivateKey", &json!([]))?
            .as_str()
            .filter(|k| is_key(k))
            .map(str::to_owned)
            .ok_or("newPrivateKey: not a hex key")?;
        store_key(&path, &key)?;
        Ok(Some(key))
    }

    fn spawn(&mut self, bootstrap_nodes: Vec<Value>) -> Result<Value, String> {
        let mut config = json!({ "bootstrapNodes": bootstrap_nodes });
        if let Some(key) = self.key()? {
            config["privKey"] = json!(key);
        }
        match (self.shim)("createNode", &json!([config.to_string()])) {
            Ok(_) => {}
            Err(e) if e.contains("already created") => {}
            Err(e) => return Err(e),
        }
        self.live = Some(Live {
            bootstrap_nodes,
            used: Instant::now(),
        });
        Ok(Value::Null)
    }

    fn create(&mut self, wanted: Vec<Value>) -> Result<Value, String> {
        let Some(live) = &mut self.live else {
            return self.spawn(wanted);
        };
        live.used = Instant::now();
        let mut all = live.bootstrap_nodes.clone();
        for node in wanted {
            if !all.iter().any(|known| known["peerId"] == node["peerId"]) {
                all.push(node);
            }
        }
        if all.len() == live.bootstrap_nodes.len() || self.calls_in_flight > 0 {
            return Ok(Value::Null);
        }
        (self.shim)("destroyNode", &json!([]))?;
        self.live = None;
        self.spawn(all)
    }
}

pub fn call(keeper: &Mutex<Keeper>, method: &str, args: &Value) -> Result<Value, String> {
    if method == "createNode" {
        let wanted = wanted_nodes(args)?;
        return lock(keeper).create(wanted);
    }
    let shim = {
        let mut keeper = lock(keeper);
        keeper.calls_in_flight += 1;
        Arc::clone(&keeper.shim)
    };
    let reply = shim(method, args);
    let mut keeper = lock(keeper);
    keeper.calls_in_flight -= 1;
    if let Some(live) = &mut keeper.live {
        live.used = Instant::now();
    }
    reply
}

pub fn retire_when_idle(keeper: &Mutex<Keeper>, idle: Duration) {
    let mut keeper = lock(keeper);
    let idle_long_enough = keeper
        .live
        .as_ref()
        .is_some_and(|live| live.used.elapsed() >= idle);
    if !idle_long_enough || keeper.calls_in_flight > 0 {
        return;
    }
    if let Err(e) = (keeper.shim)("destroyNode", &json!([])) {
        eprintln!("membership_allocation_client: the allocation node did not stop: {e}");
        return;
    }
    keeper.live = None;
}

#[cfg(not(test))]
mod own {
    use std::ffi::{c_char, CStr, CString};
    use std::path::PathBuf;
    use std::sync::{Arc, Mutex, OnceLock};
    use std::time::Duration;

    use serde_json::Value;

    use super::{call, retire_when_idle, Keeper};
    use crate::lp;

    const IDLE: Duration = Duration::from_secs(30);
    const IDLE_CHECK_EVERY: Duration = Duration::from_secs(5);

    extern "C" {
        fn allocation_node_call(method: *const c_char, args_json: *const c_char) -> *mut c_char;
        fn allocation_node_free(reply: *mut c_char);
    }

    static KEEPER: OnceLock<Mutex<Keeper>> = OnceLock::new();

    fn shim(method: &str, args: &Value) -> Result<Value, String> {
        let (Ok(method_c), Ok(args_c)) = (CString::new(method), CString::new(args.to_string()))
        else {
            return Err(format!("{method}: args not CString-safe"));
        };
        // SAFETY: both arguments are NUL-terminated and outlive the call, which
        // copies what it keeps; the reply is a heap string this side frees below.
        let raw = unsafe { allocation_node_call(method_c.as_ptr(), args_c.as_ptr()) };
        if raw.is_null() {
            return Err(format!("{method}: no reply"));
        }
        // SAFETY: a non-null reply is a NUL-terminated string that stays valid
        // until `allocation_node_free`.
        let text = unsafe { CStr::from_ptr(raw) }
            .to_string_lossy()
            .into_owned();
        // SAFETY: `raw` came from `allocation_node_call` and is freed exactly once.
        unsafe { allocation_node_free(raw) };
        let mut envelope: Value =
            serde_json::from_str(&text).map_err(|e| format!("{method}: reply parse: {e}"))?;
        if envelope.get("success").and_then(Value::as_bool) == Some(true) {
            return Ok(envelope
                .get_mut("value")
                .map(Value::take)
                .unwrap_or(Value::Null));
        }
        let error = envelope
            .get("error")
            .and_then(Value::as_str)
            .filter(|e| !e.is_empty())
            .unwrap_or("libp2p call failed");
        Err(format!("{method}: {error}"))
    }

    fn through_keeper(method: &str, args: &Value, _timeout_ms: i32) -> Result<Value, String> {
        match KEEPER.get() {
            Some(keeper) => call(keeper, method, args),
            None => Err(format!("{method}: the allocation node is not installed")),
        }
    }

    pub fn install(persistence_path: &str) {
        let key_dir = (!persistence_path.is_empty()).then(|| PathBuf::from(persistence_path));
        if key_dir.is_none() {
            eprintln!(
                "membership_allocation_client: host provided no instance_persistence_path — the allocation node takes a new peer id at every start"
            );
        }
        if KEEPER
            .set(Mutex::new(Keeper::new(Arc::new(shim), key_dir)))
            .is_err()
        {
            return;
        }
        lp::set_node(through_keeper);
        std::thread::spawn(|| loop {
            std::thread::sleep(IDLE_CHECK_EVERY);
            if let Some(keeper) = KEEPER.get() {
                retire_when_idle(keeper, IDLE);
            }
        });
    }
}

#[cfg(not(test))]
pub use own::install;

#[cfg(test)]
pub fn install(_persistence_path: &str) {}

#[cfg(test)]
mod tests {
    use std::sync::Mutex as StdMutex;

    use super::*;

    type Calls = Arc<StdMutex<Vec<(String, Value)>>>;

    fn recording(key_dir: Option<PathBuf>) -> (Mutex<Keeper>, Calls) {
        let calls = Arc::new(StdMutex::new(Vec::new()));
        let seen = Arc::clone(&calls);
        let shim: Shim = Arc::new(move |method, args| {
            seen.lock().unwrap().push((method.to_owned(), args.clone()));
            match method {
                "newPrivateKey" => Ok(json!("0a0b0c")),
                _ => Ok(Value::Null),
            }
        });
        (Mutex::new(Keeper::new(shim, key_dir)), calls)
    }

    fn methods(calls: &StdMutex<Vec<(String, Value)>>) -> Vec<String> {
        calls
            .lock()
            .unwrap()
            .iter()
            .map(|(m, _)| m.clone())
            .collect()
    }

    fn config_of(calls: &StdMutex<Vec<(String, Value)>>, at: usize) -> Value {
        let text = calls.lock().unwrap()[at].1[0].as_str().unwrap().to_owned();
        serde_json::from_str(&text).unwrap()
    }

    fn create_with(keeper: &Mutex<Keeper>, peers: &[&str]) -> Result<Value, String> {
        let nodes: Vec<Value> = peers
            .iter()
            .map(|p| json!({ "peerId": p, "addrs": ["/ip4/10.0.0.1/tcp/1"] }))
            .collect();
        call(
            keeper,
            "createNode",
            &json!([json!({ "bootstrapNodes": nodes }).to_string()]),
        )
    }

    fn scratch(tag: &str) -> PathBuf {
        let dir =
            std::env::temp_dir().join(format!("allocation-node-{tag}-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        dir
    }

    #[test]
    fn a_node_is_created_once_and_recreated_only_for_a_peer_it_lacks() {
        let (keeper, calls) = recording(None);
        create_with(&keeper, &["A"]).unwrap();
        create_with(&keeper, &["A"]).unwrap();
        assert_eq!(methods(&calls), ["createNode"]);
        assert_eq!(config_of(&calls, 0).get("privKey"), None);

        create_with(&keeper, &["B", "A"]).unwrap();
        assert_eq!(methods(&calls), ["createNode", "destroyNode", "createNode"]);
        let recreated = config_of(&calls, 2);
        let peers: Vec<&str> = recreated["bootstrapNodes"]
            .as_array()
            .unwrap()
            .iter()
            .map(|n| n["peerId"].as_str().unwrap())
            .collect();
        assert_eq!(peers, ["A", "B"]);
    }

    #[test]
    fn the_key_is_made_once_and_kept_private() {
        let dir = scratch("key");
        let (keeper, calls) = recording(Some(dir.clone()));
        create_with(&keeper, &["A"]).unwrap();
        assert_eq!(methods(&calls), ["newPrivateKey", "createNode"]);
        assert_eq!(config_of(&calls, 1)["privKey"], "0a0b0c");
        assert_eq!(fs::read_to_string(dir.join(KEY_FILE)).unwrap(), "0a0b0c");
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt as _;
            let mode = fs::metadata(dir.join(KEY_FILE))
                .unwrap()
                .permissions()
                .mode();
            assert_eq!(mode & 0o777, 0o600);
        }

        let (again, later) = recording(Some(dir.clone()));
        create_with(&again, &["A"]).unwrap();
        assert_eq!(methods(&later), ["createNode"]);
        assert_eq!(config_of(&later, 0)["privKey"], "0a0b0c");

        fs::write(dir.join(KEY_FILE), "not hex").unwrap();
        let (spoiled, none) = recording(Some(dir.clone()));
        let e = create_with(&spoiled, &["A"]).unwrap_err();
        assert!(e.contains("not a hex key"), "{e}");
        assert!(methods(&none).is_empty());
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn an_idle_node_is_retired_and_a_busy_one_is_kept() {
        let (keeper, calls) = recording(None);
        create_with(&keeper, &["A"]).unwrap();
        retire_when_idle(&keeper, Duration::from_secs(3600));
        assert_eq!(methods(&calls), ["createNode"]);

        lock(&keeper).calls_in_flight = 1;
        retire_when_idle(&keeper, Duration::ZERO);
        create_with(&keeper, &["B"]).unwrap();
        assert_eq!(methods(&calls), ["createNode"]);
        lock(&keeper).calls_in_flight = 0;

        call(&keeper, "discoLookup", &json!(["s", ""])).unwrap();
        retire_when_idle(&keeper, Duration::ZERO);
        assert_eq!(
            methods(&calls),
            ["createNode", "discoLookup", "destroyNode"]
        );
        retire_when_idle(&keeper, Duration::ZERO);
        assert_eq!(methods(&calls).len(), 3);

        create_with(&keeper, &["A"]).unwrap();
        assert_eq!(methods(&calls).last().unwrap(), "createNode");
    }
}
