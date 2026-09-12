// SPDX-License-Identifier: GPL-3.0-only
//! Three-by-three linear algebra over exact rationals.
//!
//! Colour is linear algebra, and the matrices an editor uses are derived from
//! chromaticity coordinates that are exact decimals in a standard. Doing that
//! derivation in floating point throws away the exactness at the first
//! division and then spends the rest of the pipeline hoping the error stays
//! small.
//!
//! Doing it in rationals keeps it. A BT.709 to BT.2020 matrix computed here is
//! the same on every machine, for ever, and it is the *right* matrix rather
//! than a rounding of one — which is what R-4.1 asks of a render.

use media_editor_core::Rational;

use crate::status::{RenderStatus, Result};

/// A three-element column vector.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Vector3([Rational; 3]);

impl Vector3 {
    /// A vector.
    #[must_use]
    pub const fn new(first: Rational, second: Rational, third: Rational) -> Self {
        Self([first, second, third])
    }

    /// The elements.
    #[must_use]
    pub const fn elements(&self) -> &[Rational; 3] {
        &self.0
    }

    /// One element.
    #[must_use]
    pub fn get(&self, index: usize) -> Option<Rational> {
        self.0.get(index).copied()
    }
}

/// A three-by-three matrix.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Matrix3 {
    rows: [[Rational; 3]; 3],
}

impl Matrix3 {
    /// A matrix from its rows.
    #[must_use]
    pub const fn from_rows(rows: [[Rational; 3]; 3]) -> Self {
        Self { rows }
    }

    /// The identity.
    #[must_use]
    pub const fn identity() -> Self {
        let one = Rational::ONE;
        let zero = Rational::ZERO;
        Self::from_rows([[one, zero, zero], [zero, one, zero], [zero, zero, one]])
    }

    /// A diagonal matrix.
    #[must_use]
    pub const fn diagonal(values: Vector3) -> Self {
        let zero = Rational::ZERO;
        let [first, second, third] = values.0;
        Self::from_rows([
            [first, zero, zero],
            [zero, second, zero],
            [zero, zero, third],
        ])
    }

    /// The rows.
    #[must_use]
    pub const fn rows(&self) -> &[[Rational; 3]; 3] {
        &self.rows
    }

    /// One element.
    #[must_use]
    pub fn get(&self, row: usize, column: usize) -> Option<Rational> {
        self.rows.get(row)?.get(column).copied()
    }

    /// This matrix times a vector.
    ///
    /// # Errors
    ///
    /// [`RenderStatus::Time`] wrapping an overflow, which needs values far
    /// outside anything colour produces.
    pub fn apply(&self, vector: Vector3) -> Result<Vector3> {
        let mut out = [Rational::ZERO; 3];
        for (index, row) in self.rows.iter().enumerate() {
            let mut sum = Rational::ZERO;
            for (coefficient, element) in row.iter().zip(vector.0.iter()) {
                sum = sum.checked_add(coefficient.checked_mul(*element)?)?;
            }
            out[index] = sum;
        }
        Ok(Vector3(out))
    }

    /// This matrix times another.
    ///
    /// # Errors
    ///
    /// [`RenderStatus::Time`] wrapping an overflow.
    pub fn multiply(&self, other: &Self) -> Result<Self> {
        let mut rows = [[Rational::ZERO; 3]; 3];
        for (row, out) in rows.iter_mut().enumerate() {
            for (column, cell) in out.iter_mut().enumerate() {
                let mut sum = Rational::ZERO;
                for (inner, left) in self.rows[row].iter().enumerate() {
                    let right = other.rows[inner][column];
                    sum = sum.checked_add(left.checked_mul(right)?)?;
                }
                *cell = sum;
            }
        }
        Ok(Self::from_rows(rows))
    }

    /// The determinant.
    ///
    /// # Errors
    ///
    /// [`RenderStatus::Time`] wrapping an overflow.
    pub fn determinant(&self) -> Result<Rational> {
        let m = &self.rows;
        let first = m[0][0].checked_mul(minor(m, 0, 0)?)?;
        let second = m[0][1].checked_mul(minor(m, 0, 1)?)?;
        let third = m[0][2].checked_mul(minor(m, 0, 2)?)?;
        first
            .checked_sub(second)?
            .checked_add(third)
            .map_err(Into::into)
    }

    /// The inverse, exactly.
    ///
    /// # Errors
    ///
    /// [`RenderStatus::Singular`] if the determinant is zero, or an overflow.
    pub fn inverse(&self) -> Result<Self> {
        let determinant = self.determinant()?;
        if determinant.is_zero() {
            return Err(RenderStatus::Singular);
        }
        let m = &self.rows;
        let mut rows = [[Rational::ZERO; 3]; 3];
        for (row, out) in rows.iter_mut().enumerate() {
            for (column, cell) in out.iter_mut().enumerate() {
                // The adjugate is the transpose of the cofactor matrix, so the
                // indices are swapped here on purpose.
                let cofactor = minor(m, column, row)?;
                let signed = if (row + column) % 2 == 0 {
                    cofactor
                } else {
                    cofactor.checked_neg()?
                };
                *cell = signed.checked_div(determinant)?;
            }
        }
        Ok(Self::from_rows(rows))
    }

    /// This matrix with rows and columns exchanged.
    #[must_use]
    pub fn transpose(&self) -> Self {
        let m = &self.rows;
        Self::from_rows([
            [m[0][0], m[1][0], m[2][0]],
            [m[0][1], m[1][1], m[2][1]],
            [m[0][2], m[1][2], m[2][2]],
        ])
    }
}

/// The determinant of the two-by-two matrix left when a row and column are
/// struck out.
fn minor(m: &[[Rational; 3]; 3], row: usize, column: usize) -> Result<Rational> {
    let mut values = [Rational::ZERO; 4];
    let mut index = 0;
    for (r, source) in m.iter().enumerate() {
        if r == row {
            continue;
        }
        for (c, value) in source.iter().enumerate() {
            if c == column {
                continue;
            }
            values[index] = *value;
            index += 1;
        }
    }
    let first = values[0].checked_mul(values[3])?;
    let second = values[1].checked_mul(values[2])?;
    first.checked_sub(second).map_err(Into::into)
}
