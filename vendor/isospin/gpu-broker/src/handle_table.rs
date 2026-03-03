//! Handle table for client isolation
//!
//! Each VM client gets its own handle namespace. When a client allocates
//! an object, we:
//! 1. Allocate a real handle from the NVIDIA driver
//! 2. Create a mapping from (client_id, virtual_handle) -> real_handle
//!
//! When the client references a handle, we translate it. This prevents
//! clients from accessing each other's GPU objects.

use std::collections::HashMap;
use thiserror::Error;

/// Unique identifier for a client (VM)
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct ClientId(pub u64);

/// A handle in the client's namespace (what the VM sees)
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct VirtualHandle(pub u32);

/// A handle in the real driver's namespace
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct RealHandle(pub u32);

#[derive(Debug, Error, PartialEq, Eq)]
pub enum HandleError {
    #[error("client {0:?} not found")]
    ClientNotFound(ClientId),

    #[error("handle {0:?} not found for client {1:?}")]
    HandleNotFound(VirtualHandle, ClientId),

    #[error("handle {0:?} already exists for client {1:?}")]
    HandleExists(VirtualHandle, ClientId),

    #[error("client {0:?} exceeded handle quota ({1} max)")]
    QuotaExceeded(ClientId, usize),

    #[error("real handle {0:?} already mapped for client {1:?}")]
    RealHandleConflict(RealHandle, ClientId),
}

/// Per-client handle namespace
#[derive(Debug, Clone)]
pub struct ClientHandles {
    /// Virtual handle -> Real handle
    v_to_r: HashMap<VirtualHandle, RealHandle>,

    /// Real handle -> Virtual handle (for reverse lookup)
    r_to_v: HashMap<RealHandle, VirtualHandle>,

    /// Maximum number of handles this client can have
    quota: usize,

    /// The client's root handle (NV01_ROOT_CLIENT)
    root_handle: Option<RealHandle>,
}

impl ClientHandles {
    pub fn new(quota: usize) -> Self {
        Self {
            v_to_r: HashMap::new(),
            r_to_v: HashMap::new(),
            quota,
            root_handle: None,
        }
    }

    pub fn count(&self) -> usize {
        self.v_to_r.len()
    }

    pub fn quota(&self) -> usize {
        self.quota
    }

    pub fn set_root(&mut self, real: RealHandle) {
        self.root_handle = Some(real);
    }

    pub fn root(&self) -> Option<RealHandle> {
        self.root_handle
    }

    /// Check internal invariants
    pub fn check_invariants(&self) -> Result<(), String> {
        // Invariant 1: v_to_r and r_to_v must have same size
        if self.v_to_r.len() != self.r_to_v.len() {
            return Err(format!(
                "Size mismatch: v_to_r={}, r_to_v={}",
                self.v_to_r.len(),
                self.r_to_v.len()
            ));
        }

        // Invariant 2: Every mapping in v_to_r must have inverse in r_to_v
        for (v, r) in &self.v_to_r {
            match self.r_to_v.get(r) {
                None => {
                    return Err(format!(
                        "Virtual {:?} -> Real {:?} has no reverse mapping",
                        v, r
                    ));
                }
                Some(v2) if v2 != v => {
                    return Err(format!(
                        "Virtual {:?} -> Real {:?}, but Real -> Virtual {:?}",
                        v, r, v2
                    ));
                }
                _ => {}
            }
        }

        // Invariant 3: Every mapping in r_to_v must have inverse in v_to_r
        for (r, v) in &self.r_to_v {
            match self.v_to_r.get(v) {
                None => {
                    return Err(format!(
                        "Real {:?} -> Virtual {:?} has no reverse mapping",
                        r, v
                    ));
                }
                Some(r2) if r2 != r => {
                    return Err(format!(
                        "Real {:?} -> Virtual {:?}, but Virtual -> Real {:?}",
                        r, v, r2
                    ));
                }
                _ => {}
            }
        }

        // Invariant 4: Count must not exceed quota
        if self.v_to_r.len() > self.quota {
            return Err(format!(
                "Count {} exceeds quota {}",
                self.v_to_r.len(),
                self.quota
            ));
        }

        Ok(())
    }
}

/// The main handle table - maps client handles to real driver handles
#[derive(Debug, Clone)]
pub struct HandleTable {
    /// Per-client handle mappings
    clients: HashMap<ClientId, ClientHandles>,

    /// Default quota for new clients
    default_quota: usize,
}

impl HandleTable {
    pub fn new(default_quota: usize) -> Self {
        Self {
            clients: HashMap::new(),
            default_quota,
        }
    }

    /// Register a new client
    pub fn register_client(&mut self, client: ClientId) {
        self.clients
            .entry(client)
            .or_insert_with(|| ClientHandles::new(self.default_quota));
    }

    /// Register a client with a specific quota
    pub fn register_client_with_quota(&mut self, client: ClientId, quota: usize) {
        self.clients
            .entry(client)
            .or_insert_with(|| ClientHandles::new(quota));
    }

    /// Check if a client exists
    pub fn client_exists(&self, client: ClientId) -> bool {
        self.clients.contains_key(&client)
    }

    /// Unregister a client and return all their real handles (for cleanup)
    pub fn unregister_client(&mut self, client: ClientId) -> Vec<RealHandle> {
        if let Some(handles) = self.clients.remove(&client) {
            handles.v_to_r.into_values().collect()
        } else {
            Vec::new()
        }
    }

    /// Record a new handle mapping
    pub fn insert(
        &mut self,
        client: ClientId,
        virtual_h: VirtualHandle,
        real_h: RealHandle,
    ) -> Result<(), HandleError> {
        let client_handles = self
            .clients
            .get_mut(&client)
            .ok_or(HandleError::ClientNotFound(client))?;

        if client_handles.v_to_r.contains_key(&virtual_h) {
            return Err(HandleError::HandleExists(virtual_h, client));
        }

        if client_handles.r_to_v.contains_key(&real_h) {
            return Err(HandleError::RealHandleConflict(real_h, client));
        }

        if client_handles.count() >= client_handles.quota {
            return Err(HandleError::QuotaExceeded(client, client_handles.quota));
        }

        client_handles.v_to_r.insert(virtual_h, real_h);
        client_handles.r_to_v.insert(real_h, virtual_h);
        Ok(())
    }

    /// Remove a handle mapping
    pub fn remove(
        &mut self,
        client: ClientId,
        virtual_h: VirtualHandle,
    ) -> Result<RealHandle, HandleError> {
        let client_handles = self
            .clients
            .get_mut(&client)
            .ok_or(HandleError::ClientNotFound(client))?;

        let real_h = client_handles
            .v_to_r
            .remove(&virtual_h)
            .ok_or(HandleError::HandleNotFound(virtual_h, client))?;

        client_handles.r_to_v.remove(&real_h);
        Ok(real_h)
    }

    /// Translate virtual handle to real handle
    pub fn translate(
        &self,
        client: ClientId,
        virtual_h: VirtualHandle,
    ) -> Result<RealHandle, HandleError> {
        let client_handles = self
            .clients
            .get(&client)
            .ok_or(HandleError::ClientNotFound(client))?;

        client_handles
            .v_to_r
            .get(&virtual_h)
            .copied()
            .ok_or(HandleError::HandleNotFound(virtual_h, client))
    }

    /// Reverse translate real handle to virtual handle
    pub fn reverse_translate(
        &self,
        client: ClientId,
        real_h: RealHandle,
    ) -> Result<VirtualHandle, HandleError> {
        let client_handles = self
            .clients
            .get(&client)
            .ok_or(HandleError::ClientNotFound(client))?;

        client_handles
            .r_to_v
            .get(&real_h)
            .copied()
            .ok_or_else(|| HandleError::HandleNotFound(VirtualHandle(real_h.0), client))
    }

    /// Set the root handle for a client
    pub fn set_root(&mut self, client: ClientId, real_h: RealHandle) -> Result<(), HandleError> {
        let client_handles = self
            .clients
            .get_mut(&client)
            .ok_or(HandleError::ClientNotFound(client))?;
        client_handles.set_root(real_h);
        Ok(())
    }

    /// Get the root handle for a client
    pub fn get_root(&self, client: ClientId) -> Result<Option<RealHandle>, HandleError> {
        let client_handles = self
            .clients
            .get(&client)
            .ok_or(HandleError::ClientNotFound(client))?;
        Ok(client_handles.root())
    }

    /// Get handle count for a client
    pub fn client_handle_count(&self, client: ClientId) -> Result<usize, HandleError> {
        let client_handles = self
            .clients
            .get(&client)
            .ok_or(HandleError::ClientNotFound(client))?;
        Ok(client_handles.count())
    }

    /// Get all registered client IDs
    pub fn client_ids(&self) -> Vec<ClientId> {
        self.clients.keys().copied().collect()
    }

    /// Check all invariants across all clients
    pub fn check_invariants(&self) -> Result<(), String> {
        for (client_id, handles) in &self.clients {
            handles
                .check_invariants()
                .map_err(|e| format!("Client {:?}: {}", client_id, e))?;
        }

        // Global invariant: No two clients should share a real handle
        let mut all_real_handles: HashMap<RealHandle, ClientId> = HashMap::new();
        for (client_id, handles) in &self.clients {
            for real_h in handles.r_to_v.keys() {
                if let Some(other_client) = all_real_handles.get(real_h) {
                    return Err(format!(
                        "Real handle {:?} shared between {:?} and {:?}",
                        real_h, other_client, client_id
                    ));
                }
                all_real_handles.insert(*real_h, *client_id);
            }
        }

        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_handle_isolation() {
        let mut table = HandleTable::new(1000);

        let client1 = ClientId(1);
        let client2 = ClientId(2);

        table.register_client(client1);
        table.register_client(client2);

        // Client 1 creates handle 100 -> real 5000
        table
            .insert(client1, VirtualHandle(100), RealHandle(5000))
            .unwrap();

        // Client 2 creates handle 100 -> real 6000 (same virtual, different real)
        table
            .insert(client2, VirtualHandle(100), RealHandle(6000))
            .unwrap();

        // Translations are isolated
        assert_eq!(
            table.translate(client1, VirtualHandle(100)).unwrap(),
            RealHandle(5000)
        );
        assert_eq!(
            table.translate(client2, VirtualHandle(100)).unwrap(),
            RealHandle(6000)
        );

        table.check_invariants().unwrap();
    }

    #[test]
    fn test_quota_enforcement() {
        let mut table = HandleTable::new(2); // Very small quota

        let client = ClientId(1);
        table.register_client(client);

        table
            .insert(client, VirtualHandle(1), RealHandle(100))
            .unwrap();
        table
            .insert(client, VirtualHandle(2), RealHandle(200))
            .unwrap();

        // Third insert should fail
        let result = table.insert(client, VirtualHandle(3), RealHandle(300));
        assert!(matches!(result, Err(HandleError::QuotaExceeded(_, 2))));

        table.check_invariants().unwrap();
    }

    #[test]
    fn test_cleanup_on_unregister() {
        let mut table = HandleTable::new(1000);

        let client = ClientId(1);
        table.register_client(client);

        table
            .insert(client, VirtualHandle(1), RealHandle(100))
            .unwrap();
        table
            .insert(client, VirtualHandle(2), RealHandle(200))
            .unwrap();

        let freed = table.unregister_client(client);
        assert_eq!(freed.len(), 2);
        assert!(freed.contains(&RealHandle(100)));
        assert!(freed.contains(&RealHandle(200)));

        // Client no longer exists
        assert!(table.translate(client, VirtualHandle(1)).is_err());

        table.check_invariants().unwrap();
    }

    #[test]
    fn test_duplicate_virtual_handle() {
        let mut table = HandleTable::new(1000);
        let client = ClientId(1);
        table.register_client(client);

        table
            .insert(client, VirtualHandle(1), RealHandle(100))
            .unwrap();

        // Same virtual handle should fail
        let result = table.insert(client, VirtualHandle(1), RealHandle(200));
        assert!(matches!(result, Err(HandleError::HandleExists(_, _))));

        table.check_invariants().unwrap();
    }

    #[test]
    fn test_duplicate_real_handle() {
        let mut table = HandleTable::new(1000);
        let client = ClientId(1);
        table.register_client(client);

        table
            .insert(client, VirtualHandle(1), RealHandle(100))
            .unwrap();

        // Same real handle should fail
        let result = table.insert(client, VirtualHandle(2), RealHandle(100));
        assert!(matches!(result, Err(HandleError::RealHandleConflict(_, _))));

        table.check_invariants().unwrap();
    }
}

#[cfg(test)]
mod proptests {
    use super::*;
    use proptest::prelude::*;
    use proptest::collection::vec;
    use std::collections::HashSet;

    // Strategies for generating test data
    fn client_id() -> impl Strategy<Value = ClientId> {
        (0u64..100).prop_map(ClientId)
    }

    fn virtual_handle() -> impl Strategy<Value = VirtualHandle> {
        any::<u32>().prop_map(VirtualHandle)
    }

    fn real_handle() -> impl Strategy<Value = RealHandle> {
        any::<u32>().prop_map(RealHandle)
    }

    /// Operations that can be performed on the handle table
    #[derive(Debug, Clone)]
    enum Op {
        RegisterClient(ClientId),
        UnregisterClient(ClientId),
        Insert(ClientId, VirtualHandle, RealHandle),
        Remove(ClientId, VirtualHandle),
        Translate(ClientId, VirtualHandle),
        ReverseTranslate(ClientId, RealHandle),
    }

    fn operation() -> impl Strategy<Value = Op> {
        prop_oneof![
            client_id().prop_map(Op::RegisterClient),
            client_id().prop_map(Op::UnregisterClient),
            (client_id(), virtual_handle(), real_handle()).prop_map(|(c, v, r)| Op::Insert(c, v, r)),
            (client_id(), virtual_handle()).prop_map(|(c, v)| Op::Remove(c, v)),
            (client_id(), virtual_handle()).prop_map(|(c, v)| Op::Translate(c, v)),
            (client_id(), real_handle()).prop_map(|(c, r)| Op::ReverseTranslate(c, r)),
        ]
    }

    proptest! {
        /// Property: Invariants hold after any sequence of operations
        #[test]
        fn invariants_always_hold(ops in vec(operation(), 0..200)) {
            let mut table = HandleTable::new(50);

            for op in ops {
                match op {
                    Op::RegisterClient(c) => {
                        table.register_client(c);
                    }
                    Op::UnregisterClient(c) => {
                        let _ = table.unregister_client(c);
                    }
                    Op::Insert(c, v, r) => {
                        let _ = table.insert(c, v, r);
                    }
                    Op::Remove(c, v) => {
                        let _ = table.remove(c, v);
                    }
                    Op::Translate(c, v) => {
                        let _ = table.translate(c, v);
                    }
                    Op::ReverseTranslate(c, r) => {
                        let _ = table.reverse_translate(c, r);
                    }
                }

                // Check invariants after every operation
                table.check_invariants().unwrap();
            }
        }

        /// Property: Insert then translate returns the same handle
        #[test]
        fn insert_then_translate(
            client_id in 0u64..10,
            virtual_h in any::<u32>(),
            real_h in any::<u32>()
        ) {
            let mut table = HandleTable::new(1000);
            let client = ClientId(client_id);
            table.register_client(client);

            let v = VirtualHandle(virtual_h);
            let r = RealHandle(real_h);

            table.insert(client, v, r).unwrap();

            prop_assert_eq!(table.translate(client, v).unwrap(), r);
            prop_assert_eq!(table.reverse_translate(client, r).unwrap(), v);

            table.check_invariants().unwrap();
        }

        /// Property: Insert then remove then translate fails
        #[test]
        fn insert_remove_translate_fails(
            client_id in 0u64..10,
            virtual_h in any::<u32>(),
            real_h in any::<u32>()
        ) {
            let mut table = HandleTable::new(1000);
            let client = ClientId(client_id);
            table.register_client(client);

            let v = VirtualHandle(virtual_h);
            let r = RealHandle(real_h);

            table.insert(client, v, r).unwrap();
            table.remove(client, v).unwrap();

            prop_assert!(table.translate(client, v).is_err());
            prop_assert!(table.reverse_translate(client, r).is_err());

            table.check_invariants().unwrap();
        }

        /// Property: Client isolation - one client cannot see another's handles
        #[test]
        fn client_isolation(
            v1 in any::<u32>(),
            v2 in any::<u32>(),
            r1 in any::<u32>(),
            r2 in any::<u32>()
        ) {
            // Ensure real handles are different
            prop_assume!(r1 != r2);

            let mut table = HandleTable::new(1000);
            let client1 = ClientId(1);
            let client2 = ClientId(2);

            table.register_client(client1);
            table.register_client(client2);

            table.insert(client1, VirtualHandle(v1), RealHandle(r1)).unwrap();
            table.insert(client2, VirtualHandle(v2), RealHandle(r2)).unwrap();

            // Client 1 can only see their handle
            prop_assert_eq!(table.translate(client1, VirtualHandle(v1)).unwrap(), RealHandle(r1));

            // Client 1 cannot see client 2's real handle via reverse translate
            prop_assert!(table.reverse_translate(client1, RealHandle(r2)).is_err());

            // If v1 != v2, client 1 cannot translate v2 (unless by coincidence they added it)
            if v1 != v2 {
                prop_assert!(table.translate(client1, VirtualHandle(v2)).is_err());
            }

            table.check_invariants().unwrap();
        }

        /// Property: Quota is strictly enforced
        #[test]
        fn quota_strictly_enforced(quota in 1usize..20, num_inserts in 1usize..50) {
            let mut table = HandleTable::new(quota);
            let client = ClientId(1);
            table.register_client(client);

            let mut successful = 0;
            for i in 0..num_inserts {
                let result = table.insert(
                    client,
                    VirtualHandle(i as u32),
                    RealHandle(i as u32 + 10000)
                );

                if result.is_ok() {
                    successful += 1;
                } else {
                    prop_assert!(matches!(result, Err(HandleError::QuotaExceeded(_, _))));
                }
            }

            prop_assert!(successful <= quota);
            prop_assert_eq!(table.client_handle_count(client).unwrap(), successful);

            table.check_invariants().unwrap();
        }

        /// Property: Unregister frees all handles
        #[test]
        fn unregister_frees_all(num_handles in 1usize..50) {
            let mut table = HandleTable::new(1000);
            let client = ClientId(1);
            table.register_client(client);

            let mut expected_real: HashSet<RealHandle> = HashSet::new();
            for i in 0..num_handles {
                let r = RealHandle(i as u32 + 1000);
                table.insert(client, VirtualHandle(i as u32), r).unwrap();
                expected_real.insert(r);
            }

            let freed = table.unregister_client(client);

            prop_assert_eq!(freed.len(), num_handles);
            let freed_set: HashSet<RealHandle> = freed.into_iter().collect();
            prop_assert_eq!(freed_set, expected_real);

            // Client should not exist anymore
            prop_assert!(!table.client_exists(client));

            table.check_invariants().unwrap();
        }

        /// Property: No real handle is shared between clients
        #[test]
        fn no_shared_real_handles(
            ops in vec((0u64..5, any::<u32>(), any::<u32>()), 0..100)
        ) {
            let mut table = HandleTable::new(1000);

            // Register all clients
            for i in 0..5 {
                table.register_client(ClientId(i));
            }

            // Track which real handles are assigned to which client
            let mut real_to_client: HashMap<RealHandle, ClientId> = HashMap::new();

            for (client_id, virtual_h, real_h) in ops {
                let client = ClientId(client_id);
                let v = VirtualHandle(virtual_h);
                let r = RealHandle(real_h);

                match table.insert(client, v, r) {
                    Ok(()) => {
                        // If insert succeeded, the real handle must not be in use by anyone else
                        if let Some(other) = real_to_client.get(&r) {
                            // This real handle was previously assigned to another client
                            // That's only ok if it was the same client (and same virtual, which would fail)
                            // or if that client was unregistered
                            if *other != client && table.client_exists(*other) {
                                // Check if the other client still has this real handle
                                if table.reverse_translate(*other, r).is_ok() {
                                    panic!("Real handle {:?} shared between {:?} and {:?}", r, other, client);
                                }
                            }
                        }
                        real_to_client.insert(r, client);
                    }
                    Err(HandleError::RealHandleConflict(_, _)) => {
                        // Expected - this real handle is already in use by this client
                    }
                    Err(_) => {
                        // Other errors are fine
                    }
                }
            }

            table.check_invariants().unwrap();
        }

        /// Property: Double insert fails
        #[test]
        fn double_insert_fails(
            client_id in 0u64..10,
            v1 in any::<u32>(),
            r1 in any::<u32>(),
            r2 in any::<u32>()
        ) {
            let mut table = HandleTable::new(1000);
            let client = ClientId(client_id);
            table.register_client(client);

            table.insert(client, VirtualHandle(v1), RealHandle(r1)).unwrap();

            // Same virtual handle, different real
            let result = table.insert(client, VirtualHandle(v1), RealHandle(r2));
            prop_assert!(matches!(result, Err(HandleError::HandleExists(_, _))));

            // Original mapping is unchanged
            prop_assert_eq!(table.translate(client, VirtualHandle(v1)).unwrap(), RealHandle(r1));

            table.check_invariants().unwrap();
        }

        /// Property: Remove nonexistent handle fails gracefully
        #[test]
        fn remove_nonexistent_fails(
            client_id in 0u64..10,
            v1 in any::<u32>()
        ) {
            let mut table = HandleTable::new(1000);
            let client = ClientId(client_id);
            table.register_client(client);

            let result = table.remove(client, VirtualHandle(v1));
            prop_assert!(matches!(result, Err(HandleError::HandleNotFound(_, _))));

            table.check_invariants().unwrap();
        }

        /// Property: Operations on unregistered client fail
        #[test]
        fn unregistered_client_fails(
            client_id in 0u64..10,
            v in any::<u32>(),
            r in any::<u32>()
        ) {
            let table = HandleTable::new(1000);
            let client = ClientId(client_id);
            // Note: NOT registered

            prop_assert!(matches!(
                table.translate(client, VirtualHandle(v)),
                Err(HandleError::ClientNotFound(_))
            ));

            // Create mutable table for insert/remove tests
            let mut table = HandleTable::new(1000);
            prop_assert!(matches!(
                table.insert(client, VirtualHandle(v), RealHandle(r)),
                Err(HandleError::ClientNotFound(_))
            ));
            prop_assert!(matches!(
                table.remove(client, VirtualHandle(v)),
                Err(HandleError::ClientNotFound(_))
            ));

            table.check_invariants().unwrap();
        }

        /// Property: Register is idempotent
        #[test]
        fn register_idempotent(client_id in 0u64..10) {
            let mut table = HandleTable::new(1000);
            let client = ClientId(client_id);

            table.register_client(client);
            table.insert(client, VirtualHandle(1), RealHandle(100)).unwrap();

            // Re-register should not lose handles
            table.register_client(client);

            prop_assert_eq!(table.translate(client, VirtualHandle(1)).unwrap(), RealHandle(100));

            table.check_invariants().unwrap();
        }

        /// Property: Handles survive across many operations
        #[test]
        fn handles_survive(ops in vec((0u32..10, 0u32..10), 0..100)) {
            let mut table = HandleTable::new(1000);
            let client = ClientId(1);
            table.register_client(client);

            // Insert a "canary" handle
            let canary_v = VirtualHandle(u32::MAX);
            let canary_r = RealHandle(u32::MAX);
            table.insert(client, canary_v, canary_r).unwrap();

            // Do lots of other operations that don't touch the canary
            for (v, r) in ops {
                if v != u32::MAX && r != u32::MAX {
                    let _ = table.insert(client, VirtualHandle(v), RealHandle(r + 1000));
                    let _ = table.remove(client, VirtualHandle(v));
                }
            }

            // Canary should still be there
            prop_assert_eq!(table.translate(client, canary_v).unwrap(), canary_r);

            table.check_invariants().unwrap();
        }

        /// Property: Count is always accurate
        #[test]
        fn count_always_accurate(ops in vec((any::<bool>(), 0u32..100), 0..200)) {
            let mut table = HandleTable::new(1000);
            let client = ClientId(1);
            table.register_client(client);

            let mut expected_count = 0;
            let mut next_real = 10000u32;

            for (is_insert, v) in ops {
                if is_insert {
                    if table.insert(client, VirtualHandle(v), RealHandle(next_real)).is_ok() {
                        expected_count += 1;
                        next_real += 1;
                    }
                } else {
                    if table.remove(client, VirtualHandle(v)).is_ok() {
                        expected_count -= 1;
                    }
                }

                prop_assert_eq!(table.client_handle_count(client).unwrap(), expected_count);
            }

            table.check_invariants().unwrap();
        }

        /// Property: Translation is deterministic
        #[test]
        fn translation_deterministic(
            client_id in 0u64..10,
            v in any::<u32>(),
            r in any::<u32>()
        ) {
            let mut table = HandleTable::new(1000);
            let client = ClientId(client_id);
            table.register_client(client);

            table.insert(client, VirtualHandle(v), RealHandle(r)).unwrap();

            // Multiple translates should return the same result
            let r1 = table.translate(client, VirtualHandle(v)).unwrap();
            let r2 = table.translate(client, VirtualHandle(v)).unwrap();
            let r3 = table.translate(client, VirtualHandle(v)).unwrap();

            prop_assert_eq!(r1, r2);
            prop_assert_eq!(r2, r3);
            prop_assert_eq!(r1, RealHandle(r));

            table.check_invariants().unwrap();
        }

        /// Adversarial: Rapid register/unregister cycles
        #[test]
        fn rapid_register_unregister(cycles in 0usize..50) {
            let mut table = HandleTable::new(10);
            let client = ClientId(42);

            for i in 0..cycles {
                table.register_client(client);

                // Add some handles
                for j in 0..5 {
                    let _ = table.insert(
                        client,
                        VirtualHandle(j + i as u32 * 100),
                        RealHandle(j + i as u32 * 100 + 10000)
                    );
                }

                table.check_invariants().unwrap();

                // Unregister
                let freed = table.unregister_client(client);
                prop_assert!(freed.len() <= 5);

                table.check_invariants().unwrap();
            }
        }

        /// Adversarial: Many clients with overlapping virtual handles
        #[test]
        fn many_clients_overlapping_handles(num_clients in 2usize..20) {
            let mut table = HandleTable::new(100);

            // Register all clients
            for i in 0..num_clients {
                table.register_client(ClientId(i as u64));
            }

            // Each client uses the same virtual handles 0..10
            for i in 0..num_clients {
                for v in 0u32..10 {
                    let client = ClientId(i as u64);
                    // Each client maps to unique real handles
                    let r = RealHandle(i as u32 * 1000 + v);
                    table.insert(client, VirtualHandle(v), r).unwrap();
                }
            }

            // Verify isolation
            for i in 0..num_clients {
                for v in 0u32..10 {
                    let client = ClientId(i as u64);
                    let expected = RealHandle(i as u32 * 1000 + v);
                    prop_assert_eq!(table.translate(client, VirtualHandle(v)).unwrap(), expected);
                }
            }

            table.check_invariants().unwrap();
        }

        /// Adversarial: Zero and max handles
        #[test]
        fn boundary_handles(client_id in 0u64..10) {
            let mut table = HandleTable::new(1000);
            let client = ClientId(client_id);
            table.register_client(client);

            // Test boundary values
            table.insert(client, VirtualHandle(0), RealHandle(0)).unwrap();
            table.insert(client, VirtualHandle(u32::MAX), RealHandle(u32::MAX)).unwrap();
            table.insert(client, VirtualHandle(1), RealHandle(u32::MAX - 1)).unwrap();

            prop_assert_eq!(table.translate(client, VirtualHandle(0)).unwrap(), RealHandle(0));
            prop_assert_eq!(table.translate(client, VirtualHandle(u32::MAX)).unwrap(), RealHandle(u32::MAX));

            table.check_invariants().unwrap();
        }

        /// Adversarial: Zero quota
        #[test]
        fn zero_quota(client_id in 0u64..10) {
            let mut table = HandleTable::new(0);
            let client = ClientId(client_id);
            table.register_client(client);

            // Should fail immediately
            let result = table.insert(client, VirtualHandle(1), RealHandle(1));
            prop_assert!(matches!(result, Err(HandleError::QuotaExceeded(_, 0))));

            table.check_invariants().unwrap();
        }

        /// Adversarial: Interleaved client operations
        #[test]
        fn interleaved_operations(
            ops in vec((0u64..3, 0u32..20, 0u32..1000), 0..300)
        ) {
            let mut table = HandleTable::new(100);

            // Register clients
            for i in 0..3 {
                table.register_client(ClientId(i));
            }

            for (client_id, v, r) in ops {
                let client = ClientId(client_id);

                // Randomly insert or remove
                if r % 3 == 0 {
                    let _ = table.insert(client, VirtualHandle(v), RealHandle(r + client_id as u32 * 10000));
                } else if r % 3 == 1 {
                    let _ = table.remove(client, VirtualHandle(v));
                } else {
                    let _ = table.translate(client, VirtualHandle(v));
                }

                // Invariants must hold after every operation
                table.check_invariants().unwrap();
            }
        }

        // =============================================================================
        // SECURITY-CRITICAL ADVERSARIAL TESTS
        // =============================================================================

        /// Security: Handle confusion attack - try to make client A's handle
        /// point to client B's real resource
        #[test]
        fn handle_confusion_attack(
            v in any::<u32>(),
            r in any::<u32>()
        ) {
            let mut table = HandleTable::new(1000);
            let victim = ClientId(1);
            let attacker = ClientId(2);

            table.register_client(victim);
            table.register_client(attacker);

            // Victim creates a resource
            table.insert(victim, VirtualHandle(v), RealHandle(r)).unwrap();

            // Attacker tries various ways to access victim's real handle

            // 1. Try to translate victim's virtual handle as attacker
            prop_assert!(table.translate(attacker, VirtualHandle(v)).is_err());

            // 2. Try to reverse-translate victim's real handle as attacker
            prop_assert!(table.reverse_translate(attacker, RealHandle(r)).is_err());

            // 3. Try to remove victim's handle as attacker
            prop_assert!(table.remove(attacker, VirtualHandle(v)).is_err());

            // Victim's handle should still be intact
            prop_assert_eq!(table.translate(victim, VirtualHandle(v)).unwrap(), RealHandle(r));

            table.check_invariants().unwrap();
        }

        /// Security: Use-after-free pattern - access handle after client unregistered
        #[test]
        fn use_after_free_client(
            v in any::<u32>(),
            r in any::<u32>()
        ) {
            let mut table = HandleTable::new(1000);
            let client = ClientId(1);

            table.register_client(client);
            table.insert(client, VirtualHandle(v), RealHandle(r)).unwrap();

            // Unregister the client
            let freed = table.unregister_client(client);
            prop_assert!(freed.contains(&RealHandle(r)));

            // All operations on the client should fail
            prop_assert!(matches!(
                table.translate(client, VirtualHandle(v)),
                Err(HandleError::ClientNotFound(_))
            ));
            prop_assert!(matches!(
                table.insert(client, VirtualHandle(v + 1), RealHandle(r + 1)),
                Err(HandleError::ClientNotFound(_))
            ));
            prop_assert!(matches!(
                table.remove(client, VirtualHandle(v)),
                Err(HandleError::ClientNotFound(_))
            ));

            table.check_invariants().unwrap();
        }

        /// Security: ABA problem - handle reuse after remove
        #[test]
        fn aba_handle_reuse(
            v in any::<u32>(),
            r1 in any::<u32>(),
            r2 in any::<u32>()
        ) {
            prop_assume!(r1 != r2);

            let mut table = HandleTable::new(1000);
            let client = ClientId(1);
            table.register_client(client);

            // Insert A
            table.insert(client, VirtualHandle(v), RealHandle(r1)).unwrap();
            prop_assert_eq!(table.translate(client, VirtualHandle(v)).unwrap(), RealHandle(r1));

            // Remove A
            let removed = table.remove(client, VirtualHandle(v)).unwrap();
            prop_assert_eq!(removed, RealHandle(r1));

            // Insert B with same virtual handle but different real handle
            table.insert(client, VirtualHandle(v), RealHandle(r2)).unwrap();

            // Must get B, not A
            prop_assert_eq!(table.translate(client, VirtualHandle(v)).unwrap(), RealHandle(r2));

            // r1 should not be accessible anymore
            prop_assert!(table.reverse_translate(client, RealHandle(r1)).is_err());

            table.check_invariants().unwrap();
        }

        /// Security: Real handle stealing - attacker tries to claim victim's real handle
        #[test]
        fn real_handle_stealing(
            v1 in any::<u32>(),
            v2 in any::<u32>(),
            r in any::<u32>()
        ) {
            let mut table = HandleTable::new(1000);
            let victim = ClientId(1);
            let attacker = ClientId(2);

            table.register_client(victim);
            table.register_client(attacker);

            // Victim creates a resource with real handle r
            table.insert(victim, VirtualHandle(v1), RealHandle(r)).unwrap();

            // Attacker tries to create a mapping to the same real handle
            // This should fail because each client has their own namespace
            // and the broker should never allow two clients to share a real handle
            let result = table.insert(attacker, VirtualHandle(v2), RealHandle(r));

            // This must succeed because clients have separate namespaces
            // The BROKER is responsible for ensuring real handles are unique globally
            // The handle table only ensures isolation within its tracked mappings
            prop_assert!(result.is_ok());

            // But the invariant checker should catch shared real handles
            // Wait - actually this is the BROKER's job to ensure unique real handles
            // The handle table is just tracking mappings. Let's verify invariants still hold.
            let inv_result = table.check_invariants();
            prop_assert!(inv_result.is_err()); // Should fail - real handle shared!
        }

        /// Security: Exhaustion attack - one client tries to exhaust quotas
        #[test]
        fn exhaustion_attack(quota in 1usize..100) {
            let mut table = HandleTable::new(quota);
            let attacker = ClientId(1);
            let victim = ClientId(2);

            table.register_client(attacker);
            table.register_client(victim);

            // Attacker fills up their quota
            for i in 0..quota {
                table.insert(
                    attacker,
                    VirtualHandle(i as u32),
                    RealHandle(i as u32 + 10000)
                ).unwrap();
            }

            // Attacker is at quota
            prop_assert!(table.insert(
                attacker,
                VirtualHandle(quota as u32),
                RealHandle(quota as u32 + 10000)
            ).is_err());

            // Victim should still be able to allocate
            prop_assert!(table.insert(
                victim,
                VirtualHandle(0),
                RealHandle(0)
            ).is_ok());

            table.check_invariants().unwrap();
        }

        /// Security: Integer overflow in handle values
        #[test]
        fn integer_overflow_handles(
            base in any::<u32>()
        ) {
            let mut table = HandleTable::new(1000);
            let client = ClientId(1);
            table.register_client(client);

            // Test wrapping arithmetic
            let v1 = VirtualHandle(base);
            let v2 = VirtualHandle(base.wrapping_add(1));
            let r1 = RealHandle(base);
            let r2 = RealHandle(base.wrapping_add(1));

            table.insert(client, v1, r1).unwrap();
            
            if base != base.wrapping_add(1) {
                table.insert(client, v2, r2).unwrap();
                
                prop_assert_eq!(table.translate(client, v1).unwrap(), r1);
                prop_assert_eq!(table.translate(client, v2).unwrap(), r2);
            }

            table.check_invariants().unwrap();
        }

        /// Security: Null/zero handle edge cases
        #[test]
        fn null_handle_edge_cases(client_id in 0u64..10) {
            let mut table = HandleTable::new(1000);
            let client = ClientId(client_id);
            table.register_client(client);

            // Zero handles are valid
            table.insert(client, VirtualHandle(0), RealHandle(0)).unwrap();
            prop_assert_eq!(table.translate(client, VirtualHandle(0)).unwrap(), RealHandle(0));

            // Remove it
            table.remove(client, VirtualHandle(0)).unwrap();

            // Re-add with different real
            table.insert(client, VirtualHandle(0), RealHandle(1)).unwrap();
            prop_assert_eq!(table.translate(client, VirtualHandle(0)).unwrap(), RealHandle(1));

            table.check_invariants().unwrap();
        }

        /// Security: Client ID spoofing - ensure client IDs are properly checked
        #[test]
        fn client_id_spoofing(
            real_client_id in 0u64..10,
            fake_client_id in 10u64..20,
            v in any::<u32>(),
            r in any::<u32>()
        ) {
            let mut table = HandleTable::new(1000);
            let real_client = ClientId(real_client_id);
            let fake_client = ClientId(fake_client_id);

            // Only register the real client
            table.register_client(real_client);

            // Real client creates a handle
            table.insert(real_client, VirtualHandle(v), RealHandle(r)).unwrap();

            // Fake client should not be able to access anything
            prop_assert!(matches!(
                table.translate(fake_client, VirtualHandle(v)),
                Err(HandleError::ClientNotFound(_))
            ));

            table.check_invariants().unwrap();
        }

        /// Security: Rapid allocation/deallocation stress test
        #[test]
        fn rapid_alloc_dealloc_stress(
            ops in vec((any::<bool>(), 0u32..100), 0..1000)
        ) {
            let mut table = HandleTable::new(50);
            let client = ClientId(1);
            table.register_client(client);

            let mut allocated: HashSet<u32> = HashSet::new();
            let mut next_real = 10000u32;

            for (is_alloc, v) in ops {
                if is_alloc {
                    if !allocated.contains(&v) {
                        if table.insert(client, VirtualHandle(v), RealHandle(next_real)).is_ok() {
                            allocated.insert(v);
                            next_real = next_real.wrapping_add(1);
                        }
                    }
                } else {
                    if allocated.contains(&v) {
                        if table.remove(client, VirtualHandle(v)).is_ok() {
                            allocated.remove(&v);
                        }
                    }
                }

                // Verify all allocated handles are still valid
                for v in &allocated {
                    prop_assert!(table.translate(client, VirtualHandle(*v)).is_ok());
                }

                table.check_invariants().unwrap();
            }
        }

        /// Security: Cross-client handle migration attempt
        #[test]
        fn cross_client_handle_migration(
            v in any::<u32>(),
            r in any::<u32>()
        ) {
            let mut table = HandleTable::new(1000);
            let client1 = ClientId(1);
            let client2 = ClientId(2);

            table.register_client(client1);
            table.register_client(client2);

            // Client 1 creates handle
            table.insert(client1, VirtualHandle(v), RealHandle(r)).unwrap();

            // Client 1 "tells" client 2 about the handle (simulating info leak)
            // Client 2 tries to use it - should fail
            prop_assert!(table.translate(client2, VirtualHandle(v)).is_err());

            // Client 2 tries to remove it - should fail
            prop_assert!(table.remove(client2, VirtualHandle(v)).is_err());

            // Original handle still works
            prop_assert_eq!(table.translate(client1, VirtualHandle(v)).unwrap(), RealHandle(r));

            table.check_invariants().unwrap();
        }

        /// Security: Ensure error messages don't leak information
        #[test]
        fn error_messages_no_leak(
            v in any::<u32>(),
            r in any::<u32>()
        ) {
            let mut table = HandleTable::new(1000);
            let client1 = ClientId(1);
            let client2 = ClientId(2);

            table.register_client(client1);
            table.register_client(client2);

            table.insert(client1, VirtualHandle(v), RealHandle(r)).unwrap();

            // When client2 tries to access client1's handle, the error should
            // not reveal that the handle exists for another client
            match table.translate(client2, VirtualHandle(v)) {
                Err(HandleError::HandleNotFound(vh, cid)) => {
                    // Error only mentions what client2 asked for
                    prop_assert_eq!(vh, VirtualHandle(v));
                    prop_assert_eq!(cid, client2);
                }
                other => {
                    prop_assert!(false, "Expected HandleNotFound, got {:?}", other);
                }
            }
        }

        /// Security: Mass unregistration doesn't affect other clients  
        #[test]
        fn mass_unregistration_isolation(num_clients in 2usize..20) {
            let mut table = HandleTable::new(100);

            // Register all clients and give them handles
            for i in 0..num_clients {
                table.register_client(ClientId(i as u64));
                for j in 0u32..10 {
                    table.insert(
                        ClientId(i as u64),
                        VirtualHandle(j),
                        RealHandle(i as u32 * 1000 + j)
                    ).unwrap();
                }
            }

            // Unregister all except the last one
            for i in 0..(num_clients - 1) {
                table.unregister_client(ClientId(i as u64));
            }

            // Last client's handles should still work
            let last = ClientId((num_clients - 1) as u64);
            for j in 0u32..10 {
                let expected = RealHandle((num_clients - 1) as u32 * 1000 + j);
                prop_assert_eq!(
                    table.translate(last, VirtualHandle(j)).unwrap(),
                    expected
                );
            }

            table.check_invariants().unwrap();
        }
    }
}
