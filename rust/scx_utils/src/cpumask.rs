// Copyright (c) Meta Platforms, Inc. and affiliates.

// This software may be used and distributed according to the terms of the
// GNU General Public License version 2.

//! # SCX Cpumask
//!
//! A crate that allows creating, reading, and manipulating cpumasks.
//!
//! Cpumask
//! -------
//!
//! A Cpumask object is simply a BitVec of u64's, along with a series of helper
//! functions for creating, manipulating, and reading these BitVec objects.
//!
//! Empty Cpumasks can be created directly, or they can be created from a
//! hexadecimal string:
//!
//!```
//!     let all_zeroes = Cpumask::new();
//!     let str = "0xff00ff00";
//!     let from_str_mask = Cpumask::from_string(str);
//!```
//!
//! A Cpumask can be queried and updated using its helper functions:
//!
//!```
//!     info!("{}", mask); // 32:<11111111000000001111111100000000>
//!     assert!(!mask.test_cpu(0));
//!     mask.set_cpu(0);
//!     assert!(mask.test_cpu(0));
//!
//!     mask.clear();
//!     info!("{}", mask); // 32:<00000000000000000000000000000000>
//!     assert!(!mask.test_cpu(0));
//!
//!     mask.setall();
//!     info!("{}", mask); // 32:<11111111111111111111111111111111>
//!     assert!(mask.test_cpu(0));
//!```

use anyhow::bail;
use anyhow::Context;
use anyhow::Result;
use bitvec::prelude::*;
use std::fmt;

#[derive(Debug, Clone)]
pub struct Cpumask {
    mask: BitVec<u64, Lsb0>,
    nr_cpus: usize,
}

impl Cpumask {
    fn get_cpus_possible() -> usize {
        libbpf_rs::num_possible_cpus().expect("Could not query # CPUs")
    }

    fn check_cpu(&self, cpu: usize) -> Result<()> {
        if cpu >= self.nr_cpus {
            bail!("Invalid CPU {} passed, max {}", cpu, self.nr_cpus);
        }

        Ok(())
    }

    /// Build a new empty Cpumask object.
    pub fn new() -> Result<Cpumask> {
        let nr_cpus = Cpumask::get_cpus_possible();

        Ok(Cpumask {
            mask: bitvec![u64, Lsb0; 0; nr_cpus],
            nr_cpus,
        })
    }

    /// Build a Cpumask object from a hexadecimal string.
    pub fn from_str(cpumask: &String) -> Result<Cpumask> {
        let nr_cpus = Cpumask::get_cpus_possible();

        let hex_str = {
            let mut tmp_str = cpumask
                .strip_prefix("0x")
                .unwrap_or(cpumask)
                .replace('_', "");
            if tmp_str.len() % 2 != 0 {
                tmp_str = "0".to_string() + &tmp_str;
            }
            tmp_str
        };
        let byte_vec = hex::decode(&hex_str)
            .with_context(|| format!("Failed to parse cpumask: {}", cpumask))?;

        let mut mask = bitvec![u64, Lsb0; 0; nr_cpus];
        for (index, &val) in byte_vec.iter().rev().enumerate() {
            let mut v = val;
            while v != 0 {
                let lsb = v.trailing_zeros() as usize;
                v &= !(1 << lsb);
                let cpu = index * 8 + lsb;
                if cpu > nr_cpus {
                    bail!(
                        concat!(
                            "Found cpu ({}) in cpumask ({}) which is larger",
                            " than the number of cpus on the machine ({})"
                        ),
                        cpu,
                        cpumask,
                        nr_cpus
                    );
                }
                mask.set(cpu, true);
            }
        }

        Ok(Self {
            mask,
            nr_cpus,
        })
    }

    /// Return a slice of u64's whose bits reflect the Cpumask.
    pub fn as_raw_slice(&self) -> &[u64] {
        self.mask.as_raw_slice()
    }

    /// Return the mutable raw BitVec object backing the Cpumask.
    pub fn as_raw_bitvec_mut(&mut self) -> &mut BitVec<u64, Lsb0> {
        &mut self.mask
    }

    /// Return the raw BitVec object backing the Cpumask.
    pub fn as_raw_bitvec(&self) -> &BitVec<u64, Lsb0> {
        &self.mask
    }

    /// Set all bits in the Cpumask to 1
    pub fn setall(&mut self) {
        self.mask.fill(true);
    }

    /// Set all bits in the Cpumask to 0
    pub fn clear(&mut self) {
        self.mask.fill(false);
    }

    /// Set a bit in the Cpumask. Returns an error if the specified CPU exceeds
    /// the size of the Cpumask.
    pub fn set_cpu(&mut self, cpu: usize) -> Result<()> {
        self.check_cpu(cpu)?;
        self.mask.set(cpu, true);
        Ok(())
    }

    /// Clear a bit from the Cpumask. Returns an error if the specified CPU
    /// exceeds the size of the Cpumask.
    pub fn clear_cpu(&mut self, cpu: usize) -> Result<()> {
        self.check_cpu(cpu)?;
        self.mask.set(cpu, false);
        Ok(())
    }

    /// Test whether the specified CPU bit is set in the Cpumask. If the CPU
    /// exceeds the number of possible CPUs on the host, false is returned.
    pub fn test_cpu(&self, cpu: usize) -> bool {
        match self.mask.get(cpu) {
            Some(bit) => {
                *bit
            }
            None => {
                false
            }
        }
    }

    /// Count the number of bits set in the Cpumask.
    pub fn weight(&self) -> usize {
        self.mask.count_ones()
    }

    /// The total size of the cpumask.
    pub fn len(&self) -> usize {
        self.nr_cpus
    }

    /// Create a Cpumask that is the OR of the current Cpumask and another.
    pub fn or(&self, other: &Cpumask) -> Result<Cpumask> {
        let mut new = self.clone();
        new.mask |= other.mask.clone();
        Ok(new)
    }

    /// Create a Cpumask that is the AND of the current Cpumask and another.
    pub fn and(&self, other: &Cpumask) -> Result<Cpumask> {
        let mut new = self.clone();
        new.mask &= other.mask.clone();
        Ok(new)
    }

    /// Create a Cpumask that is the XOR of the current Cpumask and another.
    pub fn xor(&self, other: &Cpumask) -> Result<Cpumask> {
        let mut new = self.clone();
        new.mask ^= other.mask.clone();
        Ok(new)
    }
}

impl fmt::Display for Cpumask {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}:<{}>", self.nr_cpus, self.mask)
    }
}

pub struct CpumaskIntoIterator {
    mask: Cpumask,
    index: usize,
}

/// Iterate over each element of a Cpumask, and return the indices with bits
/// set.
///
/// # Examples
///
/// ```
/// let mask = Cpumask::from_str(cpumask_str)?;
/// for cpu in mask.clone().into_iter() {
///     info!("cpu {} was set", cpu);
/// }
/// ```
impl IntoIterator for Cpumask {
    type Item = usize;
    type IntoIter = CpumaskIntoIterator;

    fn into_iter(self) -> CpumaskIntoIterator {
        CpumaskIntoIterator { mask: self, index: 0 }
    }
}

impl Iterator for CpumaskIntoIterator {
    type Item = usize;

    fn next(&mut self) -> Option<Self::Item> {
        while self.index < self.mask.nr_cpus {
            let index = self.index;
            self.index += 1;
            let bit_val = self.mask.test_cpu(index);
            if bit_val {
                return Some(index);
            }
        }

        None
    }
}
