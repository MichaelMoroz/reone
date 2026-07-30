/*
 * Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// Debug views, grouped behind one struct. Surface views (categories through
// albedo) replace shading at the primary hit; channel views inspect the NRD
// output split after it exists; the motion view renders the vectors
// themselves. All numbering matches the Path tracing panel's combo.

import tracing.brdf;
import tracing.resources;
import tracing.material;
import tracing.outputs;

public static const uint kPtDebugOff = 0;
public static const uint kPtDebugCategories = 1;
public static const uint kPtDebugEmissive = 2;
public static const uint kPtDebugNormals = 3;
public static const uint kPtDebugRoughness = 4;
public static const uint kPtDebugMetallic = 5;
public static const uint kPtDebugLightmap = 6;
public static const uint kPtDebugAlbedo = 7;
public static const uint kPtDebugDiffuseChannel = 8;
public static const uint kPtDebugSpecularChannel = 9;
public static const uint kPtDebugViewZ = 10;
public static const uint kPtDebugNoiseFree = 11;
public static const uint kPtDebugMotion = 12;

static const float3 kCategoryPalette[9] = {
    float3(0.5, 0.5, 0.5),  // GUI - never traced
    float3(0.3, 0.5, 0.9),  // Room
    float3(0.9, 0.3, 0.3),  // Creature
    float3(0.3, 0.9, 0.4),  // Placeable
    float3(0.9, 0.2, 0.9),  // Door
    float3(0.9, 0.8, 0.2),  // Equipment
    float3(0.5, 0.5, 0.5),  // Projectile - never traced
    float3(0.5, 0.5, 0.5),  // Camera - never traced
    float3(0.9, 0.5, 0.1),  // no model root
};

public struct PtDebug {
    public uint view;

    public static PtDebug make() {
        PtDebug debug;
        debug.view = ptDebugView();
        return debug;
    }

    public bool active() { return view != kPtDebugOff; }

    /** Views 1-6 replace shading entirely at the primary hit. */
    public bool wantsSurfaceView() {
        return view >= kPtDebugCategories && view <= kPtDebugAlbedo;
    }

    /** Views 7-11 inspect the output split at the end of the kernel. */
    public bool wantsChannelView() { return view >= kPtDebugDiffuseChannel; }

    /** A baked background has no hit record but remains the sky category. */
    public float3 skyMissColor() { return float3(0.1, 0.8, 0.9); }

    public float3 surfaceColor(HitGeometry hit, SurfaceShading shading, float3 rayDirection) {
        float shape = 0.35 + 0.65 * abs(dot(shading.normal, -rayDirection));
        switch (view) {
        case kPtDebugCategories: {
            uint category = (instanceMaterials[hit.materialIndex].featureMask >> kPtCategoryShift) & kPtCategoryMask;
            float3 color = kCategoryPalette[min(category, 8u)] * shape;
            if ((instanceMaterials[hit.materialIndex].featureMask & kPtMaskSky) != 0) color = float3(0.1, 0.8, 0.9);
            return color;
        }
        case kPtDebugEmissive: {
            float emissiveLuma = dot(shading.emission, float3(0.2126, 0.7152, 0.0722));
            return emissiveLuma > 0.001
                       ? float3(1.0, 0.25 + 0.75 * saturate(emissiveLuma), 0.0)
                       : dot(shading.albedo, float3(0.333, 0.333, 0.333)) * shape * float3(0.35);
        }
        case kPtDebugNormals:
            return shading.normal * 0.5 + 0.5;
        // Raw, deliberately: these two are the values NRD is handed, and the
        // point of looking at them is to read the number off the screen.
        // Multiplying by `shape` - as roughness used to - turns a data channel
        // into a lit render and makes a wrong value indistinguishable from a
        // steep surface. Normals and albedo above are unshaded for the same
        // reason; only the categorical views get the shape cue.
        case kPtDebugRoughness:
            return float3(shading.roughness);
        case kPtDebugMetallic:
            return float3(shading.metallic);
        case kPtDebugLightmap:
            return shading.hasLightmap ? ptLinearToGamma(shading.lightmap)
                                       : float3(0.9, 0.0, 0.9) * shape * 0.4;
        default:
            return ptLinearToGamma(shading.albedo);
        }
    }

    /** Returns true and sets color when a channel view replaces the image. */
    public bool channelColor(TraceOutputs outputs, out float3 color) {
        switch (view) {
        case kPtDebugDiffuseChannel:
            color = outputs.diffuse;
            return true;
        case kPtDebugSpecularChannel:
            color = outputs.specular;
            return true;
        case kPtDebugViewZ:
            color = float3(saturate(abs(outputs.viewZ) / 100.0));
            return true;
        case kPtDebugNoiseFree:
            color = outputs.noiseFree;
            return true;
        case kPtDebugMotion:
            // xy around mid-grey, magnitude in blue: statics read flat,
            // movers light up.
            color = float3(saturate(outputs.motion.x * 4.0 + 0.5),
                           saturate(outputs.motion.y * 4.0 + 0.5),
                           saturate(length(outputs.motion) * 4.0));
            return true;
        default:
            color = float3(0.0);
            return false;
        }
    }

    /** ViewZ and motion views bypass the display transform. */
    public bool rawChannelView() {
        return view == kPtDebugViewZ || view == kPtDebugMotion;
    }
};
