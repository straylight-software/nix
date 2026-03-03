//! Invariant definitions and checking
//!
//! These invariants are checked in debug builds and can be verified
//! via property tests. They define the correctness criteria for the protocol.

#[cfg(not(feature = "std"))]
use core::result::Result;

#[cfg(feature = "std")]
use std::result::Result;

/// An invariant that must hold
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Invariant {
    /// Pending request count is within bounds
    PendingCountBounded,
    
    /// All pending request IDs are unique
    PendingIdsUnique,
    
    /// All pending IDs are less than next_request_id
    PendingIdsLessThanNext,
    
    /// Every response matches a pending request
    ResponseMatchesPending,
    
    /// Request IDs are monotonically increasing
    RequestIdsMonotonic,
    
    /// Payload size is within bounds
    PayloadSizeBounded,
    
    /// Wire format is valid
    WireFormatValid,
}

/// Error when an invariant is violated
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum InvariantError {
    /// Pending count exceeds maximum
    PendingCountExceeded { count: usize, max: usize },
    
    /// Duplicate pending request ID
    DuplicatePendingId(u64),
    
    /// Pending ID exceeds next request ID
    PendingIdExceedsNext { pending_id: u64, next_id: u64 },
    
    /// Response for unknown request
    ResponseWithoutRequest(u64),
    
    /// Request ID went backwards
    RequestIdNotMonotonic { prev: u64, curr: u64 },
    
    /// Payload too large
    PayloadTooLarge { size: usize, max: usize },
    
    /// Invalid wire format
    InvalidWireFormat(&'static str),
}

/// Check a list of invariants against a client state
/// Returns Ok(()) if all hold, or the first violation
pub fn check_invariants(
    invariants: &[Invariant],
    checker: &impl InvariantChecker,
) -> Result<(), InvariantError> {
    for inv in invariants {
        checker.check(*inv)?;
    }
    Ok(())
}

/// Trait for types that can be checked for invariants
pub trait InvariantChecker {
    fn check(&self, invariant: Invariant) -> Result<(), InvariantError>;
}

/// All invariants that should hold for the guest protocol
pub const ALL_INVARIANTS: &[Invariant] = &[
    Invariant::PendingCountBounded,
    Invariant::PendingIdsUnique,
    Invariant::PendingIdsLessThanNext,
    Invariant::RequestIdsMonotonic,
    Invariant::PayloadSizeBounded,
    Invariant::WireFormatValid,
];

/// Invariants that should be checked on every request
pub const REQUEST_INVARIANTS: &[Invariant] = &[
    Invariant::PendingCountBounded,
    Invariant::PayloadSizeBounded,
    Invariant::WireFormatValid,
];

/// Invariants that should be checked on every response
pub const RESPONSE_INVARIANTS: &[Invariant] = &[
    Invariant::ResponseMatchesPending,
    Invariant::PayloadSizeBounded,
    Invariant::WireFormatValid,
];

#[cfg(test)]
mod tests {
    use super::*;
    
    struct MockChecker {
        should_fail: Option<Invariant>,
    }
    
    impl InvariantChecker for MockChecker {
        fn check(&self, invariant: Invariant) -> Result<(), InvariantError> {
            if self.should_fail == Some(invariant) {
                Err(InvariantError::InvalidWireFormat("mock failure"))
            } else {
                Ok(())
            }
        }
    }
    
    #[test]
    fn test_check_invariants_pass() {
        let checker = MockChecker { should_fail: None };
        assert!(check_invariants(ALL_INVARIANTS, &checker).is_ok());
    }
    
    #[test]
    fn test_check_invariants_fail() {
        let checker = MockChecker { 
            should_fail: Some(Invariant::PendingIdsUnique) 
        };
        let result = check_invariants(ALL_INVARIANTS, &checker);
        assert!(result.is_err());
    }
}
