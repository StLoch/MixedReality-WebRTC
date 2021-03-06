// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

using System;
using System.Runtime.InteropServices;

namespace Microsoft.MixedReality.WebRTC.Interop
{
    /// <summary>
    /// Handle to a native external video track source object.
    /// </summary>
    internal sealed class ExternalVideoTrackSourceHandle : SafeHandle
    {
        /// <summary>
        /// Check if the current handle is invalid, which means it is not referencing
        /// an actual native object. Note that a valid handle only means that the internal
        /// handle references a native object, but does not guarantee that the native
        /// object is still accessible. It is only safe to access the native object if
        /// the handle is not closed, which implies it being valid.
        /// </summary>
        public override bool IsInvalid
        {
            get
            {
                return (handle == IntPtr.Zero);
            }
        }

        /// <summary>
        /// Default constructor for an invalid handle.
        /// </summary>
        public ExternalVideoTrackSourceHandle() : base(IntPtr.Zero, ownsHandle: true)
        {
        }

        /// <summary>
        /// Constructor for a valid handle referencing the given native object.
        /// </summary>
        /// <param name="handle">The valid internal handle to the native object.</param>
        public ExternalVideoTrackSourceHandle(IntPtr handle) : base(IntPtr.Zero, ownsHandle: true)
        {
            SetHandle(handle);
        }

        /// <summary>
        /// Release the native object while the handle is being closed.
        /// </summary>
        /// <returns>Return <c>true</c> if the native object was successfully released.</returns>
        protected override bool ReleaseHandle()
        {
            ExternalVideoTrackSourceInterop.ExternalVideoTrackSource_RemoveRef(handle);
            return true;
        }
    }

    internal class ExternalVideoTrackSourceInterop
    {
        #region Unmanaged delegates

        // Note - Those methods cannot use SafeHandle with reverse P/Invoke; use IntPtr instead.

        [UnmanagedFunctionPointer(CallingConvention.StdCall, CharSet = CharSet.Ansi)]
        public unsafe delegate void RequestExternalI420AVideoFrameCallback(IntPtr userData,
            /*ExternalVideoTrackSourceHandle*/IntPtr sourceHandle, uint requestId, long timestampMs);

        [UnmanagedFunctionPointer(CallingConvention.StdCall, CharSet = CharSet.Ansi)]
        public unsafe delegate void RequestExternalArgb32VideoFrameCallback(IntPtr userData,
            /*ExternalVideoTrackSourceHandle*/IntPtr sourceHandle, uint requestId, long timestampMs);

        [MonoPInvokeCallback(typeof(RequestExternalI420AVideoFrameCallback))]
        public static void RequestI420AVideoFrameFromExternalSourceCallback(IntPtr userData,
            /*ExternalVideoTrackSourceHandle*/IntPtr sourceHandle, uint requestId, long timestampMs)
        {
            var args = Utils.ToWrapper<I420AVideoFrameRequestCallbackArgs>(userData);
            var request = new FrameRequest
            {
                Source = args.Source,
                RequestId = requestId,
                TimestampMs = timestampMs
            };
            args.FrameRequestCallback.Invoke(in request);
        }

        [MonoPInvokeCallback(typeof(RequestExternalArgb32VideoFrameCallback))]
        public static void RequestArgb32VideoFrameFromExternalSourceCallback(IntPtr userData,
            /*ExternalVideoTrackSourceHandle*/IntPtr sourceHandle, uint requestId, long timestampMs)
        {
            var args = Utils.ToWrapper<Argb32VideoFrameRequestCallbackArgs>(userData);
            var request = new FrameRequest
            {
                Source = args.Source,
                RequestId = requestId,
                TimestampMs = timestampMs
            };
            args.FrameRequestCallback.Invoke(in request);
        }

        #endregion


        public class VideoFrameRequestCallbackArgs
        {
            public PeerConnection Peer;
            public ExternalVideoTrackSource Source;
        }

        public class I420AVideoFrameRequestCallbackArgs : VideoFrameRequestCallbackArgs
        {
            public I420AVideoFrameRequestDelegate FrameRequestCallback;
            public RequestExternalI420AVideoFrameCallback TrampolineCallback; // keep delegate alive
        }

        public class Argb32VideoFrameRequestCallbackArgs : VideoFrameRequestCallbackArgs
        {
            public Argb32VideoFrameRequestDelegate FrameRequestCallback;
            public RequestExternalArgb32VideoFrameCallback TrampolineCallback; // keep delegate alive
        }


        #region Native functions

        [DllImport(Utils.dllPath, CallingConvention = CallingConvention.StdCall, CharSet = CharSet.Ansi,
            EntryPoint = "mrsExternalVideoTrackSourceCreateFromI420ACallback")]
        public static unsafe extern uint ExternalVideoTrackSource_CreateFromI420ACallback(
            RequestExternalI420AVideoFrameCallback callback, IntPtr userData, out ExternalVideoTrackSourceHandle sourceHandle);

        [DllImport(Utils.dllPath, CallingConvention = CallingConvention.StdCall, CharSet = CharSet.Ansi,
            EntryPoint = "mrsExternalVideoTrackSourceCreateFromArgb32Callback")]
        public static unsafe extern uint ExternalVideoTrackSource_CreateFromArgb32Callback(
            RequestExternalArgb32VideoFrameCallback callback, IntPtr userData, out ExternalVideoTrackSourceHandle sourceHandle);

        [DllImport(Utils.dllPath, CallingConvention = CallingConvention.StdCall, CharSet = CharSet.Ansi,
            EntryPoint = "mrsExternalVideoTrackSourceFinishCreation")]
        public static unsafe extern uint ExternalVideoTrackSource_FinishCreation(ExternalVideoTrackSourceHandle sourceHandle);

        [DllImport(Utils.dllPath, CallingConvention = CallingConvention.StdCall, CharSet = CharSet.Ansi,
            EntryPoint = "mrsExternalVideoTrackSourceAddRef")]
        public static unsafe extern void ExternalVideoTrackSource_AddRef(ExternalVideoTrackSourceHandle handle);

        // Note - This is used during SafeHandle.ReleaseHandle(), so cannot use ExternalVideoTrackSourceHandle
        [DllImport(Utils.dllPath, CallingConvention = CallingConvention.StdCall, CharSet = CharSet.Ansi,
            EntryPoint = "mrsExternalVideoTrackSourceRemoveRef")]
        public static unsafe extern void ExternalVideoTrackSource_RemoveRef(IntPtr handle);

        [DllImport(Utils.dllPath, CallingConvention = CallingConvention.StdCall, CharSet = CharSet.Ansi,
            EntryPoint = "mrsExternalVideoTrackSourceCompleteI420AFrameRequest")]
        public static unsafe extern uint ExternalVideoTrackSource_CompleteFrameRequest(ExternalVideoTrackSourceHandle handle,
            uint requestId, long timestampMs, in I420AVideoFrame frame);

        [DllImport(Utils.dllPath, CallingConvention = CallingConvention.StdCall, CharSet = CharSet.Ansi,
            EntryPoint = "mrsExternalVideoTrackSourceCompleteArgb32FrameRequest")]
        public static unsafe extern uint ExternalVideoTrackSource_CompleteFrameRequest(ExternalVideoTrackSourceHandle handle,
            uint requestId, long timestampMs, in Argb32VideoFrame frame);

        [DllImport(Utils.dllPath, CallingConvention = CallingConvention.StdCall, CharSet = CharSet.Ansi,
            EntryPoint = "mrsExternalVideoTrackSourceShutdown")]
        public static extern void ExternalVideoTrackSource_Shutdown(ExternalVideoTrackSourceHandle handle);

        #endregion


        #region Helpers

        public static ExternalVideoTrackSource CreateExternalVideoTrackSourceFromI420ACallback(I420AVideoFrameRequestDelegate frameRequestCallback)
        {
            // Create some static callback args which keep the sourceDelegate alive
            var args = new I420AVideoFrameRequestCallbackArgs
            {
                Peer = null, // set when track added
                Source = null, // set below
                FrameRequestCallback = frameRequestCallback,
                // This wraps the method into a temporary System.Delegate object, which is then assigned to
                // the field to ensure it is kept alive. The native callback registration below then use that
                // particular System.Delegate instance.
                TrampolineCallback = RequestI420AVideoFrameFromExternalSourceCallback
            };
            var argsRef = Utils.MakeWrapperRef(args);

            try
            {
                // A video track source starts in capturing state, so will immediately call the frame callback,
                // which requires the source to be set. So create the source wrapper first.
                var source = new ExternalVideoTrackSource(argsRef);
                args.Source = source;

                // Create the external video track source
                uint res = ExternalVideoTrackSource_CreateFromI420ACallback(args.TrampolineCallback, argsRef,
                    out ExternalVideoTrackSourceHandle sourceHandle);
                    Utils.ThrowOnErrorCode(res);
                source.OnCreated(sourceHandle);
                ExternalVideoTrackSource_FinishCreation(sourceHandle);
                return source;
            }
            catch (Exception e)
            {
                Utils.ReleaseWrapperRef(argsRef);
                throw e;
            }
        }

        public static ExternalVideoTrackSource CreateExternalVideoTrackSourceFromArgb32Callback(Argb32VideoFrameRequestDelegate frameRequestCallback)
        {
            // Create some static callback args which keep the sourceDelegate alive
            var args = new Argb32VideoFrameRequestCallbackArgs
            {
                Peer = null, // set when track added
                Source = null, // set below
                FrameRequestCallback = frameRequestCallback,
                // This wraps the method into a temporary System.Delegate object, which is then assigned to
                // the field to ensure it is kept alive. The native callback registration below then use that
                // particular System.Delegate instance.
                TrampolineCallback = RequestArgb32VideoFrameFromExternalSourceCallback
            };
            var argsRef = Utils.MakeWrapperRef(args);

            try
            {
                // A video track source starts in capturing state, so will immediately call the frame callback,
                // which requires the source to be set. So create the source wrapper first.
                var source = new ExternalVideoTrackSource(argsRef);
                args.Source = source;

                // Create the external video track source
                uint res = ExternalVideoTrackSource_CreateFromArgb32Callback(args.TrampolineCallback, argsRef,
                    out ExternalVideoTrackSourceHandle sourceHandle);
                Utils.ThrowOnErrorCode(res);
                source.OnCreated(sourceHandle);
                ExternalVideoTrackSource_FinishCreation(sourceHandle);
                return source;
            }
            catch (Exception e)
            {
                Utils.ReleaseWrapperRef(argsRef);
                throw e;
            }
        }

        public static void CompleteFrameRequest(ExternalVideoTrackSourceHandle sourceHandle, uint requestId,
            long timestampMs, in I420AVideoFrame frame)
        {
            uint res = ExternalVideoTrackSource_CompleteFrameRequest(sourceHandle, requestId, timestampMs, frame);
            Utils.ThrowOnErrorCode(res);
        }

        public static void CompleteFrameRequest(ExternalVideoTrackSourceHandle sourceHandle, uint requestId,
            long timestampMs, in Argb32VideoFrame frame)
        {
            uint res = ExternalVideoTrackSource_CompleteFrameRequest(sourceHandle, requestId, timestampMs, frame);
            Utils.ThrowOnErrorCode(res);
        }

        #endregion
    }
}
