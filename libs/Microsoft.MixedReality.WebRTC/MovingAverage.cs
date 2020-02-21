// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

using System.Collections.Generic;
using System.Diagnostics;

namespace Microsoft.MixedReality.WebRTC
{
    /// <summary>
    /// A constant time moving average 
    /// </summary>
    public class MovingAverage
    {
        private float[] window;
        private float total;
        private int numSamples;
        private int insertionIndex;
        private int period;

        /// <summary>
        /// Creates a moving average with a fixed period.
        /// </summary>
        /// <param name="period"></param>
        public MovingAverage(int period)
        {
            this.period = period;
            window = new float[period];
            Clear();
        }

        /// <summary>
        /// Adds a new entry to the average, rmeoving the oldest if needed.
        /// </summary>
        /// <param name="sample"></param>
        public void Push(float sample)
        {
            // Advance the insertion index.
            if (this.numSamples != 0)
            {
                this.insertionIndex++;
                if (this.insertionIndex == this.period)
                {
                    this.insertionIndex = 0;
                }
            }

            if (this.numSamples < period)
            {
                this.numSamples++;
            }
            else
            {
                this.total -= this.window[this.insertionIndex];
            }

            this.window[this.insertionIndex] = sample;
            this.total += sample;

            Debug.Assert(!float.IsNaN(this.total));
        }

        /// <summary>
        /// Resets the moving average and clears all samples.
        /// </summary>
        public void Clear()
        {
            this.total = 0;
            this.numSamples = 0;
            this.insertionIndex = 0;
        }

        /// <summary>
        /// Gets the average value.
        /// </summary>
        public float Average
        {
            get
            {
                return this.numSamples > 0
                    ? (this.total / this.numSamples)
                    : 0;
            }
        }
    }
}
