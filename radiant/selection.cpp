/*
   Copyright (C) 2001-2006, William Joseph.
   All Rights Reserved.

   This file is part of GtkRadiant.

   GtkRadiant is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   GtkRadiant is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with GtkRadiant; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "selection.h"

#include "debugging/debugging.h"

#include <map>
#include <list>
#include <set>

#include "windowobserver.h"
#include "iundo.h"
#include "ientity.h"
#include "cullable.h"
#include "renderable.h"
#include "selectable.h"
#include "editable.h"

#include "math/frustum.h"
#include "signal/signal.h"
#include "generic/object.h"
#include "selectionlib.h"
#include "render.h"
#include "view.h"
#include "renderer.h"
#include "stream/stringstream.h"
#include "eclasslib.h"
#include "generic/bitfield.h"
#include "generic/static.h"
#include "pivot.h"
#include "stringio.h"
#include "container/container.h"

#include "grid.h"

typedef Vector2 DeviceVector;

int g_SELECT_EPSILON = 12;

struct Pivot2World
{
	Matrix4 m_worldSpace;
	Matrix4 m_viewpointSpace;
	Matrix4 m_viewplaneSpace;
	Vector3 m_axis_screen;

	void update( const Matrix4& pivot2world, const Matrix4& modelview, const Matrix4& projection, const Matrix4& viewport ){
		Pivot2World_worldSpace( m_worldSpace, pivot2world, modelview, projection, viewport );
		Pivot2World_viewpointSpace( m_viewpointSpace, m_axis_screen, pivot2world, modelview, projection, viewport );
		Pivot2World_viewplaneSpace( m_viewplaneSpace, pivot2world, modelview, projection, viewport );
	}
};


inline Vector3 point_for_device_point( const Matrix4& device2object, const DeviceVector xy, const float z ){
	// transform from normalised device coords to object coords
	return vector4_projected( matrix4_transformed_vector4( device2object, Vector4( xy.x(), xy.y(), z, 1 ) ) );
}

inline Ray ray_for_device_point( const Matrix4& device2object, const DeviceVector xy ){
	return ray_for_points( point_for_device_point( device2object, xy, -1 ),	// point at x, y, zNear
	                       point_for_device_point( device2object, xy, 0 )		// point at x, y, zFar
	                       //point_for_device_point( device2object, xy, 1 ) //sometimes is inaccurate up to negative ray direction
	                     );
}

inline Vector3 sphere_intersect_ray( const Vector3& origin, float radius, const Ray& ray ){
	const Vector3 intersection = vector3_subtracted( origin, ray.origin );
	const double a = vector3_dot( intersection, ray.direction );
	const double d = radius * radius - ( vector3_dot( intersection, intersection ) - a * a );

	if ( d > 0 ) {
		return vector3_added( ray.origin, vector3_scaled( ray.direction, a - sqrt( d ) ) );
//		return true;
	}
	else
	{
		return vector3_added( ray.origin, vector3_scaled( ray.direction, a ) );
//		return false;
	}
}

inline Vector3 ray_intersect_ray( const Ray& ray, const Ray& other ){
	const Vector3 intersection = vector3_subtracted( ray.origin, other.origin );
	//float a = 1;//vector3_dot( ray.direction, ray.direction );        // always >= 0
	const double dot = vector3_dot( ray.direction, other.direction );
	//float c = 1;//vector3_dot( other.direction, other.direction );        // always >= 0
	const double d = vector3_dot( ray.direction, intersection );
	const double e = vector3_dot( other.direction, intersection );
	const double D = 1 - dot * dot; //a*c - dot*dot;       // always >= 0

	if ( D < 0.000001 ) {
		// the lines are almost parallel
		return vector3_added( other.origin, vector3_scaled( other.direction, e ) );
	}
	else
	{
		return vector3_added( other.origin, vector3_scaled( other.direction, ( e - dot * d ) / D ) );
	}
}

const Vector3 g_origin( 0, 0, 0 );
const float g_radius = 64;

inline Vector3 point_on_sphere( const Matrix4& device2object, const DeviceVector xy, const float radius = g_radius ){
	return sphere_intersect_ray( g_origin,
	                             radius,
	                             ray_for_device_point( device2object, xy ) );
}

inline Vector3 point_on_axis( const Vector3& axis, const Matrix4& device2object, const DeviceVector xy ){
	return ray_intersect_ray( ray_for_device_point( device2object, xy ),
	                          Ray( Vector3( 0, 0, 0 ), axis ) );
}

inline Vector3 point_on_plane( const Matrix4& device2object, const DeviceVector xy ){
	const Matrix4 object2device( matrix4_full_inverse( device2object ) );
	return vector4_projected( matrix4_transformed_vector4( device2object, Vector4( xy.x(), xy.y(), object2device[14] / object2device[15], 1 ) ) );
}

inline Vector3 point_on_plane( const Plane3& plane, const Matrix4& object2device, const DeviceVector xy ){
	return ray_intersect_plane( ray_for_device_point( matrix4_full_inverse( object2device ), xy ),
	                            plane );
}

//! a and b are unit vectors .. returns angle in radians
inline float angle_between( const Vector3& a, const Vector3& b ){
	return static_cast<float>( 2.0 * atan2(
	                               vector3_length( vector3_subtracted( a, b ) ),
	                               vector3_length( vector3_added( a, b ) )
	                           ) );
}


#if defined( _DEBUG ) && !defined( _DEBUG_QUICKER )
class test_quat
{
public:
	test_quat( const Vector3& from, const Vector3& to ){
		Vector4 quaternion( quaternion_for_unit_vectors( from, to ) );
		Matrix4 matrix( matrix4_rotation_for_quaternion( quaternion_multiplied_by_quaternion( quaternion, c_quaternion_identity ) ) );
	}
private:
};

static test_quat bleh( g_vector3_axis_x, g_vector3_axis_y );
#endif

//! axis is a unit vector
inline void constrain_to_axis( Vector3& vec, const Vector3& axis ){
	vec = vector3_normalised( vector3_added( vec, vector3_scaled( axis, -vector3_dot( vec, axis ) ) ) );
}

//! a and b are unit vectors .. a and b must be orthogonal to axis .. returns angle in radians
inline float angle_for_axis( const Vector3& a, const Vector3& b, const Vector3& axis ){
	if ( vector3_dot( axis, vector3_cross( a, b ) ) > 0.0 ) {
		return angle_between( a, b );
	}
	else{
		return -angle_between( a, b );
	}
}

inline float distance_for_axis( const Vector3& a, const Vector3& b, const Vector3& axis ){
	return static_cast<float>( vector3_dot( b, axis ) - vector3_dot( a, axis ) );
}


class ModifierFlagsExt : public ModifierFlags
{
public:
	ModifierFlagsExt( const ModifierFlags& other ) : ModifierFlags( other ){}
	bool shift() const {
		return bitfield_enabled( *this, c_modifierShift );
	}
	bool ctrl() const {
		return bitfield_enabled( *this, c_modifierControl );
	}
	bool alt() const {
		return bitfield_enabled( *this, c_modifierAlt );
	}
};

static ModifierFlagsExt g_modifiers = c_modifierNone;

class Manipulatable
{
public:
	virtual void Construct( const Matrix4& device2manip, const DeviceVector device_point, const AABB& bounds, const Vector3& transform_origin ) = 0;
	virtual void Transform( const Matrix4& manip2object, const Matrix4& device2manip, const DeviceVector device_point ) = 0;

	inline static const View* m_view = 0;
	inline static DeviceVector m_device_point;
	inline static DeviceVector m_device_epsilon;
	static void assign_static( const View& view, const DeviceVector& device_point, const DeviceVector& device_epsilon ){
		m_view = &view;
		m_device_point = device_point;
		m_device_epsilon = device_epsilon;
	}
};

inline Matrix4 transform_local2object( const Matrix4& local, const Matrix4& local2object ){
	return matrix4_multiplied_by_matrix4(
	           matrix4_multiplied_by_matrix4( local2object, local ),
	           matrix4_full_inverse( local2object )
	       );
}
inline Matrix4 transform_local2object( const Matrix4& localTransform, const Matrix4& local2parent, const Matrix4& parent2local ){
	return matrix4_multiplied_by_matrix4(
	           matrix4_multiplied_by_matrix4( local2parent, localTransform ),
	           parent2local
	       );
}

class Rotatable
{
public:
	virtual void rotate( const Quaternion& rotation ) = 0;
};

class RotateFree : public Manipulatable
{
	Vector3 m_start;
	Rotatable& m_rotatable;
public:
	RotateFree( Rotatable& rotatable )
		: m_rotatable( rotatable ){
	}
	void Construct( const Matrix4& device2manip, const DeviceVector device_point, const AABB& bounds, const Vector3& transform_origin ) override {
		m_start = point_on_sphere( device2manip, device_point );
		vector3_normalise( m_start );
	}
	void Transform( const Matrix4& manip2object, const Matrix4& device2manip, const DeviceVector device_point ) override {
		Vector3 current = point_on_sphere( device2manip, device_point );
		vector3_normalise( current );

		if( g_modifiers.shift() )
			for( std::size_t i = 0; i < 3; ++i )
				if( current[i] == 0.f )
					return m_rotatable.rotate( quaternion_for_axisangle( g_vector3_axes[i], float_snapped( angle_for_axis( m_start, current, g_vector3_axes[i] ), static_cast<float>( c_pi / 12.0 ) ) ) );

		m_rotatable.rotate( quaternion_for_unit_vectors( m_start, current ) );
	//	m_rotatable.rotate( quaternion_for_sphere_vectors( m_start, current ) ); //wrong math, 2x more sensitive
	}
};

class RotateAxis : public Manipulatable
{
	Vector3 m_axis;
	Vector3 m_start;
	float m_radius;
	bool m_plane_way;
	Plane3 m_plane;
	Vector3 m_origin;
	Rotatable& m_rotatable;
public:
	RotateAxis( Rotatable& rotatable )
		: m_radius( g_radius ), m_rotatable( rotatable ){
	}
	void Construct( const Matrix4& device2manip, const DeviceVector device_point, const AABB& bounds, const Vector3& transform_origin ) override {
		const float dot = vector3_dot( m_axis, m_view->fill()? vector3_normalised( m_view->getViewer() - transform_origin ) : m_view->getViewDir() );
		m_plane_way = fabs( dot ) > 0.1;

		if( m_plane_way ){
			m_origin = transform_origin;
			m_plane = Plane3( m_axis, vector3_dot( m_axis, m_origin ) );
			m_start = point_on_plane( m_plane, m_view->GetViewMatrix(), device_point ) - m_origin;
			vector3_normalise( m_start );
		}
		else{
			m_start = point_on_sphere( device2manip, device_point, m_radius );
			constrain_to_axis( m_start, m_axis );
		}
	}
	/// \brief Converts current position to a normalised vector orthogonal to axis.
	void Transform( const Matrix4& manip2object, const Matrix4& device2manip, const DeviceVector device_point ) override {
		Vector3 current;
		if( m_plane_way ){
			current = point_on_plane( m_plane, m_view->GetViewMatrix(), device_point ) - m_origin;
			vector3_normalise( current );
		}
		else{
			current = point_on_sphere( device2manip, device_point, m_radius );
			constrain_to_axis( current, m_axis );
		}

		if( g_modifiers.shift() ){
			m_rotatable.rotate( quaternion_for_axisangle( m_axis, float_snapped( angle_for_axis( m_start, current, m_axis ), static_cast<float>( c_pi / 12.0 ) ) ) );
		}
		else{
			m_rotatable.rotate( quaternion_for_axisangle( m_axis, angle_for_axis( m_start, current, m_axis ) ) );
		}
	}

	void SetAxis( const Vector3& axis ){
		m_axis = axis;
	}
	void SetRadius( const float radius ){
		m_radius = radius;
	}
};


/// \brief snaps changed axes of \p move so that \p bounds stick to closest grid lines.
void aabb_snap_translation( Vector3& move, const AABB& bounds ){
	const Vector3 maxs( bounds.origin + bounds.extents );
	const Vector3 mins( bounds.origin - bounds.extents );
//	globalOutputStream() << "move: " << move << '\n';
	for( std::size_t i = 0; i < 3; ++i ){
		if( fabs( move[i] ) > 1e-2f ){
			const float snapto1 = float_snapped( maxs[i] + move[i], GetSnapGridSize() );
			const float snapto2 = float_snapped( mins[i] + move[i], GetSnapGridSize() );

			const float dist1 = fabs( fabs( maxs[i] + move[i] ) - fabs( snapto1 ) );
			const float dist2 = fabs( fabs( mins[i] + move[i] ) - fabs( snapto2 ) );

//			globalOutputStream() << "maxs[i] + move[i]: " << maxs[i] + move[i]  << "    snapto1: " << snapto1 << "   dist1: " << dist1 << '\n';
//			globalOutputStream() << "mins[i] + move[i]: " << mins[i] + move[i]  << "    snapto2: " << snapto2 << "   dist2: " << dist2 << '\n';
			move[i] = dist2 > dist1 ? snapto1 - maxs[i] : snapto2 - mins[i];
		}
	}
}

inline Vector3 translation_local2object( const Vector3& local, const Matrix4& local2object ){
	return matrix4_get_translation_vec3(
	           matrix4_multiplied_by_matrix4(
	               matrix4_translated_by_vec3( local2object, local ),
	               matrix4_full_inverse( local2object )
	           )
	       );
}
inline Vector3 translation_local2object( const Vector3& localTranslation, const Matrix4& local2parent, const Matrix4& parent2local ){
	return matrix4_get_translation_vec3(
	           matrix4_multiplied_by_matrix4(
	               matrix4_translated_by_vec3( local2parent, localTranslation ),
	               parent2local
	           )
	       );
}

class Translatable
{
public:
	virtual void translate( const Vector3& translation ) = 0;
};

class TranslateAxis : public Manipulatable
{
	Vector3 m_start;
	Vector3 m_axis;
	Translatable& m_translatable;
	AABB m_bounds;
public:
	TranslateAxis( Translatable& translatable )
		: m_translatable( translatable ){
	}
	void Construct( const Matrix4& device2manip, const DeviceVector device_point, const AABB& bounds, const Vector3& transform_origin ) override {
		m_start = point_on_axis( m_axis, device2manip, device_point );
		m_bounds = bounds;
	}
	void Transform( const Matrix4& manip2object, const Matrix4& device2manip, const DeviceVector device_point ) override {
		Vector3 current = point_on_axis( m_axis, device2manip, device_point );
		current = vector3_scaled( m_axis, distance_for_axis( m_start, current, m_axis ) );

		current = translation_local2object( current, manip2object );
		if( g_modifiers.ctrl() )
			aabb_snap_translation( current, m_bounds );
		else
			vector3_snap( current, GetSnapGridSize() );

		m_translatable.translate( current );
	}

	void SetAxis( const Vector3& axis ){
		m_axis = axis;
	}
};

class TranslateAxis2 : public Manipulatable
{
private:
	Vector3 m_0;
	Plane3 m_planeSelected;
	std::size_t m_axisZ;
	Plane3 m_planeZ;
	Vector3 m_startZ;
	Translatable& m_translatable;
	AABB m_bounds;
public:
	TranslateAxis2( Translatable& translatable )
		: m_translatable( translatable ){
	}
	void Construct( const Matrix4& device2manip, const DeviceVector device_point, const AABB& bounds, const Vector3& transform_origin ) override {
		m_axisZ = vector3_max_abs_component_index( m_planeSelected.normal() );
		Vector3 xydir( m_view->getViewer() - m_0 );
		xydir[m_axisZ] = 0;
		vector3_normalise( xydir );
		m_planeZ = Plane3( xydir, vector3_dot( xydir, m_0 ) );
		m_startZ = point_on_plane( m_planeZ, m_view->GetViewMatrix(), device_point );
		m_bounds = bounds;
	}
	void Transform( const Matrix4& manip2object, const Matrix4& device2manip, const DeviceVector device_point ) override {
		Vector3 current = g_vector3_axes[m_axisZ] * vector3_dot( m_planeSelected.normal(), ( point_on_plane( m_planeZ, m_view->GetViewMatrix(), device_point ) - m_startZ ) )
		                  * ( m_planeSelected.normal()[m_axisZ] >= 0? 1 : -1 );

		if( !std::isfinite( current[0] ) || !std::isfinite( current[1] ) || !std::isfinite( current[2] ) ) // catch INF case, is likely with top of the box in 2D
			return;

		if( g_modifiers.ctrl() )
			aabb_snap_translation( current, m_bounds );
		else
			vector3_snap( current, GetSnapGridSize() );

		m_translatable.translate( current );
	}
	void set0( const Vector3& start, const Plane3& planeSelected ){
		m_0 = start;
		m_planeSelected = planeSelected;
	}
};

class TranslateFree : public Manipulatable
{
private:
	Vector3 m_start;
	Translatable& m_translatable;
	AABB m_bounds;
public:
	TranslateFree( Translatable& translatable )
		: m_translatable( translatable ){
	}
	void Construct( const Matrix4& device2manip, const DeviceVector device_point, const AABB& bounds, const Vector3& transform_origin ) override {
		m_start = point_on_plane( device2manip, device_point );
		m_bounds = bounds;
	}
	void Transform( const Matrix4& manip2object, const Matrix4& device2manip, const DeviceVector device_point ) override {
		Vector3 current = point_on_plane( device2manip, device_point );
		current = vector3_subtracted( current, m_start );

		if( g_modifiers.shift() ) // snap to axis
			current *= g_vector3_axes[vector3_max_abs_component_index( current )];

		current = translation_local2object( current, manip2object );

		if( g_modifiers.ctrl() ) // snap aabb
			aabb_snap_translation( current, m_bounds );
		else
			vector3_snap( current, GetSnapGridSize() );

		m_translatable.translate( current );
	}
};


/// \brief constructs Quaternion so that rotated box geometry ends up aligned to one or more axes (depends on how much axial \p to is).
inline Quaternion quaternion_for_unit_vectors_for_bounds( const Vector3& axialfrom, const Vector3& to ){
	// do step by step from the larger component to the smaller one
	size_t ids[3] = { vector3_max_abs_component_index( to ), ( ids[0] + 1 ) %3, ( ids[0] + 2 ) %3 };
	if( std::fabs( to[ids[2]] ) > std::fabs( to[ids[1]] ) )
		std::swap( ids[2], ids[1] );

	Vector3 steps[3] = { g_vector3_axes[ids[0]] * std::copysign( 1.f, to[ids[0]] ), to, to };

	Quaternion rotation = quaternion_for_unit_vectors_safe( axialfrom, steps[0] );
	if( std::fabs( to[ids[1]] ) > 1e-6f ){
		steps[1][ids[2]] = 0;
		vector3_normalise( steps[1] );
		rotation = quaternion_multiplied_by_quaternion( quaternion_for_unit_vectors( steps[0], steps[1] ), rotation );
		if( std::fabs( to[ids[2]] ) > 1e-6f ){
			rotation = quaternion_multiplied_by_quaternion( quaternion_for_unit_vectors( steps[1], to ), rotation );
		}
	}
	return rotation;
}


class AllTransformable
{
public:
	virtual void alltransform( const Transforms& transforms, const Vector3& world_pivot ) = 0;
};

#include <optional>
struct testSelect_unselected_scene_point_return_t{ DoubleVector3 point; std::optional<Plane3> plane; };
std::optional<testSelect_unselected_scene_point_return_t>
testSelect_unselected_scene_point( const View& view, const DeviceVector device_point, const DeviceVector device_epsilon );

void Scene_BoundsSelected_withEntityBounds( scene::Graph& graph, AABB& bounds );

std::optional<Vector3> AABB_TestPoint( const View& view, const DeviceVector device_point, const DeviceVector device_epsilon, const AABB& aabb );

class SnapBounds : public Manipulatable
{
private:
	Translatable& m_translatable;
	AllTransformable& m_transformable;
	AABB m_bounds;
	Vector3 m_0;
	// rotate-snap axis and sign of aabb
	size_t m_roatateAxis = 0;
	int m_rotateSign = 1;

	std::optional<Plane3> m_along_plane;
	Vector3 m_along_plane_start_point;
public:
	SnapBounds( Translatable& translatable, AllTransformable& transformable )
		: m_translatable( translatable ), m_transformable( transformable ){
	}
	void Construct( const Matrix4& device2manip, const DeviceVector device_point, const AABB& bounds, const Vector3& transform_origin ) override {
		if( GlobalSelectionSystem().Mode() == SelectionSystem::ePrimitive )
			Scene_BoundsSelected_withEntityBounds( GlobalSceneGraph(), m_bounds );
		else
			m_bounds = bounds;

		// for rotate-snap deduce aabb side opposite to clicked
		if( const auto point = AABB_TestPoint( *m_view, device_point, m_device_epsilon, m_bounds ) ){
			m_0 = point.value(); // original m_0 is less reliable fallback
		}
		m_roatateAxis = 0;
		m_rotateSign = 1;
		float bestDist = FLT_MAX;
		for( size_t axis : { 0, 1, 2 } )
			for( int sign : { -1, 1 } )
				if( const float dist = fabs( m_0[axis] - ( m_bounds.origin[axis] + std::copysign( m_bounds.extents[axis], sign ) ) ); dist < bestDist ){
					bestDist = dist;
					m_roatateAxis = axis;
					m_rotateSign = sign;
				}

		m_along_plane.reset();
	}
	void Transform( const Matrix4& manip2object, const Matrix4& device2manip, const DeviceVector device_point ) override {
		Vector3 current( g_vector3_identity );
		if( g_modifiers.shift() ){ // move along plane
			if( !m_along_plane ){ // try to initialize plane from original cursor position
				if( const auto test = testSelect_unselected_scene_point( *m_view, m_device_point, m_device_epsilon );
					test && test->plane ){
					m_along_plane = test->plane;
					m_along_plane_start_point = point_on_plane( *m_along_plane, m_view->GetViewMatrix(), m_device_point );
				}
				else if( const auto test = testSelect_unselected_scene_point( *m_view, device_point, m_device_epsilon );
					test && test->plane ){ // init cursor pos was not on plane, try to fallback to current pos
					m_along_plane = test->plane;
					m_along_plane_start_point = point_on_plane( *m_along_plane, m_view->GetViewMatrix(), device_point );
				}
			}
			if( m_along_plane ){ // got plane, lez go
				current = point_on_plane( *m_along_plane, m_view->GetViewMatrix(), device_point ) - m_along_plane_start_point;
				const size_t maxi = vector3_max_abs_component_index( m_along_plane->normal() );
				vector3_snap( current, GetSnapGridSize() );
				// snap move on two axes with least normal component -> need to find out 3rd move component
				// it equals to point snap to plane with dist=0
				// normal.dot( snapped move ) = 0
				current[maxi] = -( m_along_plane->normal()[( maxi + 1 ) % 3] * current[( maxi + 1 ) % 3]
				                 + m_along_plane->normal()[( maxi + 2 ) % 3] * current[( maxi + 2 ) % 3] )
				                 / m_along_plane->normal()[maxi];
				return m_translatable.translate( current );
			}
		}
		else if( const auto test = testSelect_unselected_scene_point( *m_view, device_point, m_device_epsilon ) ){
			const auto choose_aabb_corner = []( const AABB& bounds, const size_t axis, const Vector3& nrm, const Vector3& ray ){
				Vector3 extents = bounds.extents;
				extents[axis] = std::copysign( extents[axis], nrm[axis] );
				extents[( axis + 1 ) % 3] = std::copysign( extents[( axis + 1 ) % 3], ray[( axis + 1 ) % 3] );
				extents[( axis + 2 ) % 3] = std::copysign( extents[( axis + 2 ) % 3], ray[( axis + 2 ) % 3] );
				return bounds.origin - extents;
			};
			const Ray ray = ray_for_device_point( matrix4_full_inverse( m_view->GetViewMatrix() ), device_point );
			const Vector3 nrm = test->plane? Vector3( test->plane->normal() ) : -ray.direction;
			if( g_modifiers.alt() ){ // rotate-snap
				const Quaternion rotation = quaternion_for_unit_vectors_for_bounds( g_vector3_axes[m_roatateAxis] * m_rotateSign, nrm );
				const Matrix4 unrot = matrix4_rotation_for_quaternion( quaternion_inverse( rotation ) );
				const Vector3 unray = matrix4_transformed_direction( unrot,
					test->plane
					? ray.direction
					// when test point has no plane data we rotate exactly to test ray... tweak ray to deduce distinct aabb corner
					: ray_for_device_point( matrix4_full_inverse( m_view->GetViewMatrix() ), device_point * 1.1f ).direction );
				const Vector3 corner = choose_aabb_corner( m_bounds, m_roatateAxis, -unray, unray );

				Transforms transforms;
				transforms.setRotation( rotation );
				transforms.setTranslation( test->point - corner );
				return m_transformable.alltransform( transforms, corner );
			}
			else{ // move-snap
				const std::size_t axis = vector3_max_abs_component_index( nrm ); // snap bbox along this axis
				current = test->point - choose_aabb_corner( m_bounds, axis, nrm, ray.direction );
				return m_translatable.translate( current );
			}
		}

		m_translatable.translate( current ); // fallback to move to original position
	}
	void set0( const Vector3& start ){
		m_0 = start;
	}
	static bool useCondition( const ModifierFlagsExt& modifiers, const View& view ){
		return modifiers.ctrl() && view.fill();
	}
};


class TranslateFreeXY_Z : public Manipulatable
{
private:
	Vector3 m_0;
	std::size_t m_axisZ;
	Plane3 m_planeXY;
	Plane3 m_planeZ;
	Vector3 m_startXY;
	Vector3 m_startZ;
	Translatable& m_translatable;
	AABB m_bounds;
	SnapBounds m_snapBounds;
public:
	static int m_viewdependent;
	TranslateFreeXY_Z( Translatable& translatable, AllTransformable& transformable )
		: m_translatable( translatable ), m_snapBounds( translatable, transformable ){
	}
	void Construct( const Matrix4& device2manip, const DeviceVector device_point, const AABB& bounds, const Vector3& transform_origin ) override {
		m_axisZ = ( m_viewdependent || !m_view->fill() )? vector3_max_abs_component_index( m_view->getViewDir() ) : 2;
		if( m_0 == g_vector3_identity ) /* special value to indicate missing good point to start with, i.e. while dragging components by clicking anywhere; m_startXY, m_startZ != m_0 in this case */
			m_0 = transform_origin;
		m_planeXY = Plane3( g_vector3_axes[m_axisZ], m_0[m_axisZ] );
#if 0
		Vector3 xydir( m_view->getViewDir() );
#else
		Vector3 xydir( m_view->getViewer() - m_0 );
#endif
		xydir[m_axisZ] = 0;
		vector3_normalise( xydir );
		m_planeZ = Plane3( xydir, vector3_dot( xydir, m_0 ) );
		m_startXY = point_on_plane( m_planeXY, m_view->GetViewMatrix(), device_point );
		m_startZ = point_on_plane( m_planeZ, m_view->GetViewMatrix(), device_point );
		m_bounds = bounds;

		m_snapBounds.Construct( device2manip, device_point, bounds, transform_origin );
	}
	void Transform( const Matrix4& manip2object, const Matrix4& device2manip, const DeviceVector device_point ) override {
		if( SnapBounds::useCondition( g_modifiers, *m_view ) ){
			m_snapBounds.Transform( manip2object, device2manip, device_point );
			return;
		}

		Vector3 current;
		if( g_modifiers.alt() && m_view->fill() ) // Z only
			current = ( point_on_plane( m_planeZ, m_view->GetViewMatrix(), device_point ) - m_startZ ) * g_vector3_axes[m_axisZ];
		else{
			current = point_on_plane( m_planeXY, m_view->GetViewMatrix(), device_point ) - m_startXY;
			current[m_axisZ] = 0;
		}

		if( g_modifiers.shift() ) // snap to axis
			current *= g_vector3_axes[vector3_max_abs_component_index( current )];

		if( g_modifiers.ctrl() ) // snap aabb
			aabb_snap_translation( current, m_bounds );
		else
			vector3_snap( current, GetSnapGridSize() );

		m_translatable.translate( current );
	}
	void set0( const Vector3& start ){
		m_0 = start;
		m_snapBounds.set0( start );
	}
};
int TranslateFreeXY_Z::m_viewdependent = 0;

class Scalable
{
public:
	virtual void scale( const Vector3& scaling ) = 0;
};


class ScaleAxis : public Manipulatable
{
private:
	Vector3 m_start;
	Vector3 m_axis;
	Scalable& m_scalable;

	Vector3 m_chosen_extent;
	AABB m_bounds;

public:
	ScaleAxis( Scalable& scalable )
		: m_scalable( scalable ){
	}
	void Construct( const Matrix4& device2manip, const DeviceVector device_point, const AABB& bounds, const Vector3& transform_origin ) override {
		m_start = point_on_axis( m_axis, device2manip, device_point );

		m_chosen_extent = Vector3(
		                       std::max( bounds.origin[0] + bounds.extents[0] - transform_origin[0], - bounds.origin[0] + bounds.extents[0] + transform_origin[0] ),
		                       std::max( bounds.origin[1] + bounds.extents[1] - transform_origin[1], - bounds.origin[1] + bounds.extents[1] + transform_origin[1] ),
		                       std::max( bounds.origin[2] + bounds.extents[2] - transform_origin[2], - bounds.origin[2] + bounds.extents[2] + transform_origin[2] )
		                   );
		m_bounds = bounds;
	}
	void Transform( const Matrix4& manip2object, const Matrix4& device2manip, const DeviceVector device_point ) override {
		//globalOutputStream() << "manip2object: " << manip2object << "  device2manip: " << device2manip << "  x: " << x << "  y:" << y << '\n';
		Vector3 current = point_on_axis( m_axis, device2manip, device_point );
		Vector3 delta = vector3_subtracted( current, m_start );

		delta = translation_local2object( delta, manip2object );
		vector3_snap( delta, GetSnapGridSize() );
		vector3_scale( delta, m_axis );

		Vector3 start( vector3_snapped( m_start, GetSnapGridSize() != 0.f ? GetSnapGridSize() : 1e-3f ) );
		for ( std::size_t i = 0; i < 3; ++i ){ //prevent snapping to 0 with big gridsize
			if( float_snapped( m_start[i], 1e-3f ) != 0.f && start[i] == 0.f ){
				start[i] = GetSnapGridSize();
			}
		}
		//globalOutputStream() << "m_start: " << m_start << "   start: " << start << "   delta: " << delta << '\n';
		/* boundless way */
		Vector3 scale(
		    start[0] == 0 ? 1 : 1 + delta[0] / start[0],
		    start[1] == 0 ? 1 : 1 + delta[1] / start[1],
		    start[2] == 0 ? 1 : 1 + delta[2] / start[2]
		);
		/* try bbox way */
		for( std::size_t i = 0; i < 3; i++ ){
			if( m_chosen_extent[i] > 0.0625f && m_axis[i] != 0.f ){ //epsilon to prevent super high scale for set of models, having really small extent, formed by origins
				scale[i] = ( m_chosen_extent[i] + delta[i] ) / m_chosen_extent[i];
				if( g_modifiers.ctrl() ){ // snap bbox dimension size to grid
					const float snappdwidth = float_snapped( scale[i] * m_bounds.extents[i] * 2.f, GetSnapGridSize() );
					scale[i] = snappdwidth / ( m_bounds.extents[i] * 2.f );
				}
			}
		}
		if( g_modifiers.shift() ){ // scale all axes equally
			for( std::size_t i = 0; i < 3; i++ ){
				if( m_axis[i] == 0.f ){
					scale[i] = vector3_dot( scale, vector3_scaled( m_axis, m_axis ) );
				}
			}
		}
		//globalOutputStream() << "scale: " << scale << '\n';
		m_scalable.scale( scale );
	}

	void SetAxis( const Vector3& axis ){
		m_axis = axis;
	}
};

class ScaleFree : public Manipulatable
{
private:
	Vector3 m_start;
	Vector3 m_axis;
	Vector3 m_axis2;
	Scalable& m_scalable;

	Vector3 m_chosen_extent;
	AABB m_bounds;

public:
	ScaleFree( Scalable& scalable )
		: m_scalable( scalable ){
	}
	void Construct( const Matrix4& device2manip, const DeviceVector device_point, const AABB& bounds, const Vector3& transform_origin ) override {
		m_start = point_on_plane( device2manip, device_point );

		m_chosen_extent = Vector3(
		                       std::max( bounds.origin[0] + bounds.extents[0] - transform_origin[0], -( bounds.origin[0] - bounds.extents[0] - transform_origin[0] ) ),
		                       std::max( bounds.origin[1] + bounds.extents[1] - transform_origin[1], -( bounds.origin[1] - bounds.extents[1] - transform_origin[1] ) ),
		                       std::max( bounds.origin[2] + bounds.extents[2] - transform_origin[2], -( bounds.origin[2] - bounds.extents[2] - transform_origin[2] ) )
		                   );
		m_bounds = bounds;
	}
	void Transform( const Matrix4& manip2object, const Matrix4& device2manip, const DeviceVector device_point ) override {
		Vector3 current = point_on_plane( device2manip, device_point );
		Vector3 delta = vector3_subtracted( current, m_start );

		delta = translation_local2object( delta, manip2object );
		vector3_snap( delta, GetSnapGridSize() );
		if( m_axis != g_vector3_identity )
			delta = vector3_scaled( delta, m_axis ) + vector3_scaled( delta, m_axis2 );

		Vector3 start( vector3_snapped( m_start, GetSnapGridSize() != 0.f ? GetSnapGridSize() : 1e-3f ) );
		for ( std::size_t i = 0; i < 3; ++i ){ //prevent snapping to 0 with big gridsize
			if( float_snapped( m_start[i], 1e-3f ) != 0.f && start[i] == 0.f ){
				start[i] = GetSnapGridSize();
			}
		}

		const std::size_t ignore_axis = vector3_min_abs_component_index( m_start );
		if( g_modifiers.shift() )
			start[ignore_axis] = 0.f;

		Vector3 scale(
		    start[0] == 0 ? 1 : 1 + delta[0] / start[0],
		    start[1] == 0 ? 1 : 1 + delta[1] / start[1],
		    start[2] == 0 ? 1 : 1 + delta[2] / start[2]
		);

		//globalOutputStream() << "m_start: " << m_start << "   start: " << start << "   delta: " << delta << '\n';
		for( std::size_t i = 0; i < 3; i++ ){
			if( m_chosen_extent[i] > 0.0625f && start[i] != 0.f ){
				scale[i] = ( m_chosen_extent[i] + delta[i] ) / m_chosen_extent[i];
				if( g_modifiers.ctrl() ){ // snap bbox dimension size to grid
					const float snappdwidth = float_snapped( scale[i] * m_bounds.extents[i] * 2.f, GetSnapGridSize() );
					scale[i] = snappdwidth / ( m_bounds.extents[i] * 2.f );
				}
			}
		}
		//globalOutputStream() << "pre snap scale: " << scale << '\n';
		if( g_modifiers.shift() ){ // snap 2 axes equally
			float bestscale = ignore_axis != 0 ? scale[0] : scale[1];
			for( std::size_t i = ignore_axis != 0 ? 1 : 2; i < 3; i++ ){
				if( ignore_axis != i && fabs( scale[i] ) < fabs( bestscale ) ){
					bestscale = scale[i];
				}
				//globalOutputStream() << "bestscale: " << bestscale << '\n';
			}
			for( std::size_t i = 0; i < 3; i++ ){
				if( ignore_axis != i ){
					scale[i] = ( scale[i] < 0.f ) ? -fabs( bestscale ) : fabs( bestscale );
				}
			}
		}
		//globalOutputStream() << "scale: " << scale << '\n';
		m_scalable.scale( scale );
	}
	void SetAxes( const Vector3& axis, const Vector3& axis2 ){
		m_axis = axis;
		m_axis2 = axis2;
	}
};


class Skewable
{
public:
	virtual void skew( const Skew& skew ) = 0;
};


class SkewAxis : public Manipulatable
{
private:
	Vector3 m_0;
	Plane3 m_planeZ;

	int m_axis_which;
	int m_axis_by;
	int m_axis_by_sign;
	Skewable& m_skewable;

	float m_axis_by_extent;
	AABB m_bounds;

public:
	SkewAxis( Skewable& skewable )
		: m_skewable( skewable ){
	}
	void Construct( const Matrix4& device2manip, const DeviceVector device_point, const AABB& bounds, const Vector3& transform_origin ) override {
		Vector3 xydir( m_view->getViewer() - m_0 );
		xydir[m_axis_which] = 0;
	//	xydir *= g_vector3_axes[vector3_max_abs_component_index( xydir )];
		vector3_normalise( xydir );
		m_planeZ = Plane3( xydir, vector3_dot( xydir, m_0 ) );

		m_bounds = bounds;
		m_axis_by_extent = bounds.origin[m_axis_by] + bounds.extents[m_axis_by] * m_axis_by_sign - transform_origin[m_axis_by];
	}
	void Transform( const Matrix4& manip2object, const Matrix4& device2manip, const DeviceVector device_point ) override {
		const Vector3 current = point_on_plane( m_planeZ, m_view->GetViewMatrix(), device_point ) - m_0;
	//	globalOutputStream() << m_axis_which << " by axis " << m_axis_by << '\n';
		m_skewable.skew( Skew( m_axis_by * 4 + m_axis_which, m_axis_by_extent != 0.f? float_snapped( current[m_axis_which], GetSnapGridSize() ) / m_axis_by_extent : 0 ) );
	}
	void SetAxes( int axis_which, int axis_by, int axis_by_sign ){
		m_axis_which = axis_which;
		m_axis_by = axis_by;
		m_axis_by_sign = axis_by_sign;
	}
	void set0( const Vector3& start ){
		m_0 = start;
	}
};

#include "brush.h"
#include "brushnode.h"
#include "brushmanip.h"

class DragNewBrush : public Manipulatable
{
private:
	Vector3 m_0;
	Vector3 m_size;
	float m_setSizeZ; /* store separately for fine square/cube modes handling */
	scene::Node* m_newBrushNode;
public:
	DragNewBrush(){
	}
	void Construct( const Matrix4& device2manip, const DeviceVector device_point, const AABB& bounds, const Vector3& transform_origin ) override {
		m_setSizeZ = m_size[0] = m_size[1] = m_size[2] = GetGridSize();
		m_newBrushNode = 0;
	}
	void Transform( const Matrix4& manip2object, const Matrix4& device2manip, const DeviceVector device_point ) override {
		Vector3 diff_raw = point_on_plane( Plane3( g_vector3_axis_z, vector3_dot( g_vector3_axis_z, Vector3( m_size.x(), m_size.y(), m_setSizeZ ) + m_0 ) ), m_view->GetViewMatrix(), device_point ) - m_0;
		const Vector3 xydir( vector3_normalised( Vector3( m_view->GetModelview()[2], m_view->GetModelview()[6], 0 ) ) );
		diff_raw.z() = ( point_on_plane( Plane3( xydir, vector3_dot( xydir, Vector3( m_size.x(), m_size.y(), m_setSizeZ ) + m_0 ) ), m_view->GetViewMatrix(), device_point ) - m_0 ).z();
		Vector3 diff = vector3_snapped( diff_raw, GetSnapGridSize() );

		for ( std::size_t i = 0; i < 3; ++i )
			if( diff[i] == 0 )
				diff[i] = diff_raw[i] < 0? -GetGridSize() : GetGridSize();

		if( g_modifiers.alt() ){ // height adjustment
			diff.x() = m_size.x();
			diff.y() = m_size.y();
		}
		else{
			diff.z() = m_size.z();
		}

		const float z = vector4_projected( matrix4_transformed_vector4( m_view->GetViewMatrix(), Vector4( diff + m_0, 1 ) ) ).z();
		if( z != z || z > 1 ) //catch NAN and behind near, far planes cases
			return;

		if( g_modifiers.shift() || g_modifiers.ctrl() ){ // square or cube
			const float squaresize = std::max( fabs( diff.x() ), fabs( diff.y() ) );
			diff.x() = diff.x() > 0? squaresize : -squaresize; //square
			diff.y() = diff.y() > 0? squaresize : -squaresize;
			if( g_modifiers.ctrl() && !g_modifiers.alt() ) //cube
				diff.z() = diff.z() > 0? squaresize : -squaresize;
		}

		m_size = diff;
		if( g_modifiers.alt() )
			m_setSizeZ = diff.z();

		Vector3 mins( m_0 );
		Vector3 maxs( m_0 + diff );
		for ( std::size_t i = 0; i < 3; ++i )
			if( mins[i] > maxs[i] )
				std::swap( mins[i], maxs[i] );

		Scene_BrushResize_Cuboid( m_newBrushNode, aabb_for_minmax( mins, maxs ) );
	}
	void set0( const Vector3& start ){
		m_0 = start;
	}
};



class DragExtrudeFaces : public Manipulatable
{
private:
	Vector3 m_0;
	Plane3 m_planeSelected;
	std::size_t m_axisZ;
	Plane3 m_planeZ;
	Vector3 m_startZ;

	bool m_originalBrushSaved;
	bool m_originalBrushChanged;

public:
	class ExtrudeSource
	{
	public:
		BrushInstance* m_brushInstance;
		struct InFaceOutBrush{
			Face* m_face;
			PlanePoints m_planepoints;
			Brush* m_outBrush;
		};
		std::vector<InFaceOutBrush> m_faces;
		std::vector<InFaceOutBrush>::iterator faceFind( const Face* face ){
			return std::find_if( m_faces.begin(), m_faces.end(), [face]( const InFaceOutBrush& infaceoutbrush ){
				return face == infaceoutbrush.m_face;
			} );
		}
		std::vector<InFaceOutBrush>::const_iterator faceFind( const Face* face ) const {
			return std::find_if( m_faces.begin(), m_faces.end(), [face]( const InFaceOutBrush& infaceoutbrush ){
				return face == infaceoutbrush.m_face;
			} );
		}
		bool faceExcluded( const Face* face ) const {
			return faceFind( face ) == m_faces.end();
		}
	};
	std::vector<ExtrudeSource> m_extrudeSources;

	DragExtrudeFaces(){
	}
	void Construct( const Matrix4& device2manip, const DeviceVector device_point, const AABB& bounds, const Vector3& transform_origin ) override {
		m_axisZ = vector3_max_abs_component_index( m_planeSelected.normal() );
		Vector3 xydir( m_view->getViewer() - m_0 );
		xydir[m_axisZ] = 0;
		vector3_normalise( xydir );
		m_planeZ = Plane3( xydir, vector3_dot( xydir, m_0 ) );
		m_startZ = point_on_plane( m_planeZ, m_view->GetViewMatrix(), device_point );

		m_originalBrushSaved = false;
		m_originalBrushChanged = false;

		UndoableCommand undo( "ExtrudeBrushFaces" );
		for( ExtrudeSource& source : m_extrudeSources ){
			for( auto& infaceoutbrush : source.m_faces ){
				const Face* face = infaceoutbrush.m_face;

				NodeSmartReference node( GlobalBrushCreator().createBrush() );
				Node_getTraversable( source.m_brushInstance->path().parent() )->insert( node );

				scene::Path path( source.m_brushInstance->path() );
				path.pop();
				path.push( makeReference( node.get() ) );
				selectPath( path, true );

				Brush* brush = Node_getBrush( node.get() );
				infaceoutbrush.m_outBrush = brush;

				Face* f = brush->addFace( *face );
				f->getPlane().offset( GetGridSize() );
				f->planeChanged();

				f = brush->addFace( *face );
				f->getPlane().reverse();
				f->planeChanged();

				for( const WindingVertex& vertex : face->getWinding() ){
					if( vertex.adjacent != c_brush_maxFaces ){
						f = brush->addFace( **std::next( source.m_brushInstance->getBrush().begin(), vertex.adjacent ) );

						const DoubleVector3 cross = vector3_cross( f->plane3_().normal(), face->plane3_().normal() );
						f->getPlane().copy( vertex.vertex, vertex.vertex + cross * 64, vertex.vertex + face->plane3_().normal() * 64 );
						f->planeChanged();
					}
				}
			}
		}
	}
	void Transform( const Matrix4& manip2object, const Matrix4& device2manip, const DeviceVector device_point ) override {
		Vector3 current = g_vector3_axes[m_axisZ] * vector3_dot( m_planeSelected.normal(), ( point_on_plane( m_planeZ, m_view->GetViewMatrix(), device_point ) - m_startZ ) )
		                  * ( m_planeSelected.normal()[m_axisZ] >= 0? 1 : -1 );

		if( !std::isfinite( current[0] ) || !std::isfinite( current[1] ) || !std::isfinite( current[2] ) ) // catch INF case, is likely with top of the box in 2D
			return;

		vector3_snap( current, GetSnapGridSize() );

		const float offset = fabs( m_planeSelected.normal()[m_axisZ] ) * std::copysign(
		                              std::max( static_cast<double>( GetGridSize() ), vector3_length( current ) ),
		                              vector3_dot( current, m_planeSelected.normal() ) );

		if( offset >= 0 ){ // extrude outside
			if( m_originalBrushChanged ){
				m_originalBrushChanged = false;
				for( ExtrudeSource& source : m_extrudeSources ){
					// revert original brush
					for( auto& infaceoutbrush : source.m_faces ){
						Face* face = infaceoutbrush.m_face;
						face->getPlane().copy( infaceoutbrush.m_planepoints );
						face->planeChanged();
					}
				}
			}
			for( ExtrudeSource& source : m_extrudeSources ){
				Brush& brush0 = source.m_brushInstance->getBrush();
				if( source.m_faces.size() > 1 ){
					Brush* tmpbrush = new Brush( brush0 );
					offsetFaces( source, *tmpbrush, offset );
					brush_extrudeDiag( brush0, *tmpbrush, source );
					delete tmpbrush;
				}
				else{
					for( auto& infaceoutbrush : source.m_faces ){
						const Face* face = infaceoutbrush.m_face;
						Brush* brush = infaceoutbrush.m_outBrush;
						brush->clear();

						Face* f = brush->addFace( *face );
						f->getPlane().offset( offset );
						f->planeChanged();

						f = brush->addFace( *face );
						f->getPlane().reverse();
						f->planeChanged();

						for( const WindingVertex& vertex : face->getWinding() ){
							if( vertex.adjacent != c_brush_maxFaces ){
								brush->addFace( **std::next( brush0.begin(), vertex.adjacent ) );
							}
						}
					}
				}
			}
		}
		else{ // extrude inside
			if( !m_originalBrushSaved ){
				m_originalBrushSaved = true;
				for( ExtrudeSource& source : m_extrudeSources )
					for( auto& infaceoutbrush : source.m_faces )
						infaceoutbrush.m_face->undoSave();
			}
			m_originalBrushChanged = true;

			for( ExtrudeSource& source : m_extrudeSources ){
				Brush& brush0 = source.m_brushInstance->getBrush();
				// revert original brush
				for( auto& infaceoutbrush : source.m_faces ){
					Face* face = infaceoutbrush.m_face;
					face->getPlane().copy( infaceoutbrush.m_planepoints );
					face->planeChanged();
				}
				if( source.m_faces.size() > 1 ){
					Brush* tmpbrush = new Brush( brush0 );
					tmpbrush->evaluateBRep();
					offsetFaces( source, brush0, offset );
					if( brush0.hasContributingFaces() )
						brush_extrudeDiag( brush0, *tmpbrush, source );
					delete tmpbrush;
				}
				else{
					for( auto& infaceoutbrush : source.m_faces ){
						Face* face = infaceoutbrush.m_face;
						Brush* brush = infaceoutbrush.m_outBrush;
						brush->clear();

						brush->copy( brush0 );

						Face* f = brush->addFace( *face );
						f->getPlane().offset( offset );
						f->getPlane().reverse();
						f->planeChanged();

						brush->removeEmptyFaces();
						// modify original brush
						face->getPlane().offset( offset );
						face->planeChanged();
					}
				}
			}
		}
	}
	void set0( const Vector3& start, const Plane3& planeSelected ){
		m_0 = start;
		m_planeSelected = planeSelected;
	}

private:
	void offsetFaces( const ExtrudeSource& source, Brush& brush, const float offset ){
		const Brush& brush0 = source.m_brushInstance->getBrush();
		for( Brush::const_iterator i0 = brush0.begin(); i0 != brush0.end(); ++i0 ){
			const Face& face0 = *( *i0 );
			if( !source.faceExcluded( &face0 ) ){
				Face& face = *( *std::next( brush.begin(), std::distance( brush0.begin(), i0 ) ) );
				face.getPlane().offset( offset );
				face.planeChanged();
			}
		}
		brush.evaluateBRep();
	}
	/* brush0, brush2 are supposed to have same amount of faces in the same order; brush2 bigger than brush0 */
	void brush_extrudeDiag( const Brush& brush0, const Brush& brush2, ExtrudeSource& source ){
		TextureProjection projection;
		TexDef_Construct_Default( projection );

		for( Brush::const_iterator i0 = brush0.begin(); i0 != brush0.end(); ++i0 ){
			const Face& face0 = *( *i0 );
			const Face& face2 = *( *std::next( brush2.begin(), std::distance( brush0.begin(), i0 ) ) );

			auto infaceoutbrush_iter = source.faceFind( &face0 ); // brush0 = source.m_brushInstance->getBrush()
			if( infaceoutbrush_iter != source.m_faces.end() ) {
				if( face0.contributes() || face2.contributes() ) {
					const char* shader = face0.GetShader();

					Brush* outBrush = ( *infaceoutbrush_iter ).m_outBrush;
					outBrush->clear();

					if( face0.contributes() ){
						if( Face* newFace = outBrush->addFace( face0 ) ) {
							newFace->flipWinding();
						}
					}
					if( face2.contributes() ){
						outBrush->addFace( face2 );
					}

					if( face0.contributes() && face2.contributes() ){ //sew two valid windings
						const auto addSidePlanes = [&outBrush, shader, &projection]( const Winding& winding0, const Winding& winding2, const DoubleVector3 normal, const bool swap ){
							for( std::size_t index0 = 0; index0 < winding0.numpoints; ++index0 ){
								const std::size_t next = Winding_next( winding0, index0 );
								DoubleVector3 BestPoint;
								double bestdot = -1;
								for( std::size_t index2 = 0; index2 < winding2.numpoints; ++index2 ){
									const double dot = vector3_dot(
									                       vector3_normalised(
									                           vector3_cross(
									                               winding0[index0].vertex - winding0[next].vertex,
									                               winding0[index0].vertex - winding2[index2].vertex
									                           )
									                       ),
									                       normal
									                   );
									if( dot > bestdot ) {
										bestdot = dot;
										BestPoint = winding2[index2].vertex;
									}
								}
								outBrush->addPlane( winding0[swap? next : index0].vertex,
								                    winding0[swap? index0 : next].vertex,
								                    BestPoint,
								                    shader,
								                    projection );
							}
						};
						//insert side planes from each winding perspective, as their form may change after brush expansion
						addSidePlanes( face0.getWinding(), face2.getWinding(), face0.getPlane().plane3().normal(), false );
						addSidePlanes( face2.getWinding(), face0.getWinding(), face0.getPlane().plane3().normal(), true );
					}
					else{ //one valid winding: this way may produce garbage with complex brushes, extruded partially, but does preferred result with simple ones
						const auto addSidePlanes = [&outBrush, shader, &projection]( const Winding& winding0, const Brush& brush2, const Plane3 plane, const bool swap ){
							for( std::size_t index0 = 0; index0 < winding0.numpoints; ++index0 ){
								const std::size_t next = Winding_next( winding0, index0 );
								DoubleVector3 BestPoint;
								double bestdist = 999999;
								for( const Face* f : brush2 ) {
									const Winding& winding2 = f->getWinding();
									for( std::size_t index2 = 0; index2 < winding2.numpoints; ++index2 ){
										const double testdist = vector3_length( winding0[index0].vertex - winding2[index2].vertex );
										if( testdist < bestdist && plane3_distance_to_point( plane, winding2[index2].vertex ) > .05 ) {
											bestdist = testdist;
											BestPoint = winding2[index2].vertex;
										}
									}
								}
								outBrush->addPlane( winding0[swap? next : index0].vertex,
								                    winding0[swap? index0 : next].vertex,
								                    BestPoint,
								                    shader,
								                    projection );
							}
						};

						if( face0.contributes() )
							addSidePlanes( face0.getWinding(), brush2, face0.getPlane().plane3(), false );
						else if( face2.contributes() )
							addSidePlanes( face2.getWinding(), brush0, plane3_flipped( face2.getPlane().plane3() ), true );
					}
					outBrush->removeEmptyFaces();
				}
			}
		}
	}
};







class RenderableClippedPrimitive : public OpenGLRenderable
{
	struct primitive_t
	{
		PointVertex m_points[9];
		std::size_t m_count;
	};
	Matrix4 m_inverse;
	std::vector<primitive_t> m_primitives;
public:
	Matrix4 m_world;

	void render( RenderStateFlags state ) const override {
		for ( std::size_t i = 0; i < m_primitives.size(); ++i )
		{
			gl().glColorPointer( 4, GL_UNSIGNED_BYTE, sizeof( PointVertex ), &m_primitives[i].m_points[0].colour );
			gl().glVertexPointer( 3, GL_FLOAT, sizeof( PointVertex ), &m_primitives[i].m_points[0].vertex );
			switch ( m_primitives[i].m_count )
			{
			case 1: break;
			case 2: gl().glDrawArrays( GL_LINES, 0, GLsizei( m_primitives[i].m_count ) ); break;
			default: gl().glDrawArrays( GL_POLYGON, 0, GLsizei( m_primitives[i].m_count ) ); break;
			}
		}
	}

	void construct( const Matrix4& world2device ){
		m_inverse = matrix4_full_inverse( world2device );
		m_world = g_matrix4_identity;
	}

	void insert( const Vector4 clipped[9], std::size_t count ){
		add_one();

		m_primitives.back().m_count = count;
		for ( std::size_t i = 0; i < count; ++i )
		{
			Vector3 world_point( vector4_projected( matrix4_transformed_vector4( m_inverse, clipped[i] ) ) );
			m_primitives.back().m_points[i].vertex = vertex3f_for_vector3( world_point );
		}
	}

	void destroy(){
		m_primitives.clear();
	}
private:
	void add_one(){
		m_primitives.push_back( primitive_t() );

		const Colour4b colour_clipped( 255, 127, 0, 255 );

		for ( std::size_t i = 0; i < 9; ++i )
			m_primitives.back().m_points[i].colour = colour_clipped;
	}
};

#if defined( _DEBUG ) && !defined( _DEBUG_QUICKER )
#define DEBUG_SELECTION
#endif

#if defined( DEBUG_SELECTION )
Shader* g_state_clipped;
RenderableClippedPrimitive g_render_clipped;
#endif

typedef Vector3 point_t;
typedef const Vector3* point_iterator_t;

// crossing number test for a point in a polygon
// This code is patterned after [Franklin, 2000]
bool point_test_polygon_2d( const point_t& P, point_iterator_t start, point_iterator_t finish ){
	std::size_t crossings = 0;

	// loop through all edges of the polygon
	for ( point_iterator_t prev = finish - 1, cur = start; cur != finish; prev = cur, ++cur )
	{	// edge from (*prev) to (*cur)
		if ( ( ( ( *prev )[1] <= P[1] ) && ( ( *cur )[1] > P[1] ) ) // an upward crossing
		  || ( ( ( *prev )[1] > P[1] ) && ( ( *cur )[1] <= P[1] ) ) ) { // a downward crossing
			// compute the actual edge-ray intersect x-coordinate
			float vt = (float)( P[1] - ( *prev )[1] ) / ( ( *cur )[1] - ( *prev )[1] );
			if ( P[0] < ( *prev )[0] + vt * ( ( *cur )[0] - ( *prev )[0] ) ) { // P[0] < intersect
				++crossings; // a valid crossing of y=P[1] right of P[0]
			}
		}
	}
	return ( crossings & 0x1 ) != 0; // 0 if even (out), and 1 if odd (in)
}

inline double triangle_signed_area_XY( const Vector3& p0, const Vector3& p1, const Vector3& p2 ){
	return ( ( p1[0] - p0[0] ) * ( p2[1] - p0[1] ) ) - ( ( p2[0] - p0[0] ) * ( p1[1] - p0[1] ) );
}

enum clipcull_t
{
	eClipCullNone,
	eClipCullCW,
	eClipCullCCW,
};


inline SelectionIntersection select_point_from_clipped( Vector4& clipped ){
	return SelectionIntersection( clipped[2] / clipped[3], static_cast<float>( vector3_length_squared( Vector3( clipped[0] / clipped[3], clipped[1] / clipped[3], 0 ) ) ) );
}

void BestPoint( std::size_t count, Vector4 clipped[9], SelectionIntersection& best, clipcull_t cull, const Plane3* plane = 0 ){
	Vector3 normalised[9];

	{
		for ( std::size_t i = 0; i < count; ++i )
		{
			normalised[i][0] = clipped[i][0] / clipped[i][3];
			normalised[i][1] = clipped[i][1] / clipped[i][3];
			normalised[i][2] = clipped[i][2] / clipped[i][3];
		}
	}

	if ( cull != eClipCullNone && count > 2 ) {
		double signed_area = triangle_signed_area_XY( normalised[0], normalised[1], normalised[2] );

		if ( ( cull == eClipCullCW && signed_area > 0 )
		  || ( cull == eClipCullCCW && signed_area < 0 ) ) {
			return;
		}
	}

	if ( count == 2 ) {
		const Vector3 point = line_closest_point( Line( normalised[0], normalised[1] ), Vector3( 0, 0, 0 ) );
		assign_if_closer( best, SelectionIntersection( point.z(), vector3_length_squared( Vector3( point.x(), point.y(), 0 ) ) ) );
	}
	else if ( count > 2 && !point_test_polygon_2d( Vector3( 0, 0, 0 ), normalised, normalised + count ) ) {
		Plane3 plaine;
		if( !plane ){
			plaine = plane3_for_points( normalised[0], normalised[1], normalised[2] );
			plane = &plaine;
		}
//globalOutputStream() << plane.a << ' ' << plane.b << ' ' << plane.c << ' ' << '\n';
		const point_iterator_t end = normalised + count;
		for ( point_iterator_t previous = end - 1, current = normalised; current != end; previous = current, ++current )
		{
			Vector3 point = line_closest_point( Line( *previous, *current ), Vector3( 0, 0, 0 ) );
			float depth = point.z();
			point.z() = 0;
			float distance = static_cast<float>( vector3_length_squared( point ) );

			if( plane->c == 0 ){
				assign_if_closer( best, SelectionIntersection( depth, distance ) );
			}
			else{
				assign_if_closer( best, SelectionIntersection( depth, distance, ray_distance_to_plane(
				                      Ray( Vector3( 0, 0, 0 ), Vector3( 0, 0, 1 ) ),
				                      *plane
				                  ) ) );
//										globalOutputStream() << static_cast<float>( ray_distance_to_plane(
//										Ray( Vector3( 0, 0, 0 ), Vector3( 0, 0, 1 ) ),
//										plane
//										) ) << '\n';
			}
		}
	}
	else if ( count > 2 ) {
		Plane3 plaine;
		if( !plane ){
			plaine = plane3_for_points( normalised[0], normalised[1], normalised[2] );
			plane = &plaine;
		}
		assign_if_closer(
		    best,
		    SelectionIntersection(
		        ray_distance_to_plane(
		            Ray( Vector3( 0, 0, 0 ), Vector3( 0, 0, 1 ) ),
		            *plane
		        ),
		        0,
		        ray_distance_to_plane(
		            Ray( Vector3( 10, 8, 0 ), Vector3( 0, 0, 1 ) ),
		            *plane
		        )
		    )
		);
	}

#if defined( DEBUG_SELECTION )
	if ( count >= 2 ) {
		g_render_clipped.insert( clipped, count );
	}
#endif
}

void Point_BestPoint( const Matrix4& local2view, const PointVertex& vertex, SelectionIntersection& best ){
	Vector4 clipped;
	if ( matrix4_clip_point( local2view, vertex3f_to_vector3( vertex.vertex ), clipped ) == c_CLIP_PASS ) {
		assign_if_closer( best, select_point_from_clipped( clipped ) );
	}
}

void LineStrip_BestPoint( const Matrix4& local2view, const PointVertex* vertices, const std::size_t size, SelectionIntersection& best ){
	Vector4 clipped[2];
	for ( std::size_t i = 0; ( i + 1 ) < size; ++i )
	{
		const std::size_t count = matrix4_clip_line( local2view, vertex3f_to_vector3( vertices[i].vertex ), vertex3f_to_vector3( vertices[i + 1].vertex ), clipped );
		BestPoint( count, clipped, best, eClipCullNone );
	}
}

void LineLoop_BestPoint( const Matrix4& local2view, const PointVertex* vertices, const std::size_t size, SelectionIntersection& best ){
	Vector4 clipped[2];
	for ( std::size_t i = 0; i < size; ++i )
	{
		const std::size_t count = matrix4_clip_line( local2view, vertex3f_to_vector3( vertices[i].vertex ), vertex3f_to_vector3( vertices[( i + 1 ) % size].vertex ), clipped );
		BestPoint( count, clipped, best, eClipCullNone );
	}
}

void Line_BestPoint( const Matrix4& local2view, const PointVertex vertices[2], SelectionIntersection& best ){
	Vector4 clipped[2];
	const std::size_t count = matrix4_clip_line( local2view, vertex3f_to_vector3( vertices[0].vertex ), vertex3f_to_vector3( vertices[1].vertex ), clipped );
	BestPoint( count, clipped, best, eClipCullNone );
}

void Circle_BestPoint( const Matrix4& local2view, clipcull_t cull, const PointVertex* vertices, const std::size_t size, SelectionIntersection& best ){
	Vector4 clipped[9];
	for ( std::size_t i = 0; i < size; ++i )
	{
		const std::size_t count = matrix4_clip_triangle( local2view, g_vector3_identity, vertex3f_to_vector3( vertices[i].vertex ), vertex3f_to_vector3( vertices[( i + 1 ) % size].vertex ), clipped );
		BestPoint( count, clipped, best, cull );
	}
}

void Quad_BestPoint( const Matrix4& local2view, clipcull_t cull, const PointVertex* vertices, SelectionIntersection& best ){
	Vector4 clipped[9];
	{
		const std::size_t count = matrix4_clip_triangle( local2view, vertex3f_to_vector3( vertices[0].vertex ), vertex3f_to_vector3( vertices[1].vertex ), vertex3f_to_vector3( vertices[3].vertex ), clipped );
		BestPoint( count, clipped, best, cull );
	}
	{
		const std::size_t count = matrix4_clip_triangle( local2view, vertex3f_to_vector3( vertices[1].vertex ), vertex3f_to_vector3( vertices[2].vertex ), vertex3f_to_vector3( vertices[3].vertex ), clipped );
		BestPoint( count, clipped, best, cull );
	}
}

void AABB_BestPoint( const Matrix4& local2view, clipcull_t cull, const AABB& aabb, SelectionIntersection& best ){
	const IndexPointer::index_type indices_[24] = {
		2, 1, 5, 6,
		1, 0, 4, 5,
		0, 1, 2, 3,
		3, 7, 4, 0,
		3, 2, 6, 7,
		7, 6, 5, 4,
	};

	const std::array<Vector3, 8> points = aabb_corners( aabb );

	const IndexPointer indices( indices_, 24 );

	Vector4 clipped[9];
	for ( IndexPointer::iterator i( indices.begin() ); i != indices.end(); i += 4 )
	{
		BestPoint(
		    matrix4_clip_triangle(
		        local2view,
		        points[*i],
		        points[*( i + 1 )],
		        points[*( i + 3 )],
		        clipped
		    ),
		    clipped,
		    best,
		    cull
		);
		BestPoint(
		    matrix4_clip_triangle(
		        local2view,
		        points[*( i + 1 )],
		        points[*( i + 2 )],
		        points[*( i + 3 )],
		        clipped
		    ),
		    clipped,
		    best,
		    cull
		);
	}
}

struct FlatShadedVertex
{
	Vertex3f vertex;
	Colour4b colour;
	Normal3f normal;

	FlatShadedVertex(){
	}
};


typedef FlatShadedVertex* FlatShadedVertexIterator;
void Triangles_BestPoint( const Matrix4& local2view, clipcull_t cull, FlatShadedVertexIterator first, FlatShadedVertexIterator last, SelectionIntersection& best ){
	for ( FlatShadedVertexIterator x( first ), y( first + 1 ), z( first + 2 ); x != last; x += 3, y += 3, z += 3 )
	{
		Vector4 clipped[9];
		BestPoint(
		    matrix4_clip_triangle(
		        local2view,
		        reinterpret_cast<const Vector3&>( ( *x ).vertex ),
		        reinterpret_cast<const Vector3&>( ( *y ).vertex ),
		        reinterpret_cast<const Vector3&>( ( *z ).vertex ),
		        clipped
		    ),
		    clipped,
		    best,
		    cull
		);
	}
}


class SelectionVolume : public SelectionTest
{
	Matrix4 m_local2view;
	const View& m_view;
	clipcull_t m_cull;
#if 0
	Vector3 m_near;
	Vector3 m_far;
#endif
	Matrix4 m_screen2world;
public:
	SelectionVolume( const View& view )
		: m_view( view ){
	}

	const VolumeTest& getVolume() const override {
		return m_view;
	}
#if 0
	const Vector3& getNear() const override {
		return m_near;
	}
	const Vector3& getFar() const override {
		return m_far;
	}
#endif
	const Matrix4& getScreen2world() const override {
		return m_screen2world;
	}

	void BeginMesh( const Matrix4& localToWorld, bool twoSided ) override {
		m_local2view = matrix4_multiplied_by_matrix4( m_view.GetViewMatrix(), localToWorld );

		// Cull back-facing polygons based on winding being clockwise or counter-clockwise.
		// Don't cull if the view is wireframe and the polygons are two-sided.
		m_cull = twoSided && !m_view.fill() ? eClipCullNone : ( matrix4_handedness( localToWorld ) == MATRIX4_RIGHTHANDED ) ? eClipCullCW : eClipCullCCW;

		{
			m_screen2world = matrix4_full_inverse( m_local2view );
#if 0
			m_near = vector4_projected(
			             matrix4_transformed_vector4(
			                 m_screen2world,
			                 Vector4( 0, 0, -1, 1 )
			             )
			         );

			m_far = vector4_projected(
			            matrix4_transformed_vector4(
			                m_screen2world,
			                Vector4( 0, 0, 1, 1 )
			            )
			        );
#endif
		}

#if defined( DEBUG_SELECTION )
		g_render_clipped.construct( m_view.GetViewMatrix() );
#endif
	}
	void TestPoint( const Vector3& point, SelectionIntersection& best ) override {
		Vector4 clipped;
		if ( matrix4_clip_point( m_local2view, point, clipped ) == c_CLIP_PASS ) {
			best = select_point_from_clipped( clipped );
		}
	}
	void TestPolygon( const VertexPointer& vertices, std::size_t count, SelectionIntersection& best, const DoubleVector3 planepoints[3] ) override {
		DoubleVector3 pts[3];
		pts[0] = vector4_projected( matrix4_transformed_vector4( m_local2view, BasicVector4<double>( planepoints[0], 1 ) ) );
		pts[1] = vector4_projected( matrix4_transformed_vector4( m_local2view, BasicVector4<double>( planepoints[1], 1 ) ) );
		pts[2] = vector4_projected( matrix4_transformed_vector4( m_local2view, BasicVector4<double>( planepoints[2], 1 ) ) );
		const Plane3 planeTransformed( plane3_for_points( pts ) );

		Vector4 clipped[9];
		for ( std::size_t i = 0; i + 2 < count; ++i )
		{
			BestPoint(
			    matrix4_clip_triangle(
			        m_local2view,
			        reinterpret_cast<const DoubleVector3&>( vertices[0] ),
			        reinterpret_cast<const DoubleVector3&>( vertices[i + 1] ),
			        reinterpret_cast<const DoubleVector3&>( vertices[i + 2] ),
			        clipped
			    ),
			    clipped,
			    best,
			    m_cull,
			    &planeTransformed
			);
		}
	}
	void TestLineLoop( const VertexPointer& vertices, std::size_t count, SelectionIntersection& best ) override {
		if ( count == 0 ) {
			return;
		}
		Vector4 clipped[9];
		for ( VertexPointer::iterator i = vertices.begin(), end = i + count, prev = i + ( count - 1 ); i != end; prev = i, ++i )
		{
			BestPoint(
			    matrix4_clip_line(
			        m_local2view,
			        reinterpret_cast<const Vector3&>( ( *prev ) ),
			        reinterpret_cast<const Vector3&>( ( *i ) ),
			        clipped
			    ),
			    clipped,
			    best,
			    m_cull
			);
		}
	}
	void TestLineStrip( const VertexPointer& vertices, std::size_t count, SelectionIntersection& best ) override {
		if ( count == 0 ) {
			return;
		}
		Vector4 clipped[9];
		for ( VertexPointer::iterator i = vertices.begin(), end = i + count, next = i + 1; next != end; i = next, ++next )
		{
			BestPoint(
			    matrix4_clip_line(
			        m_local2view,
			        reinterpret_cast<const Vector3&>( ( *i ) ),
			        reinterpret_cast<const Vector3&>( ( *next ) ),
			        clipped
			    ),
			    clipped,
			    best,
			    m_cull
			);
		}
	}
	void TestLines( const VertexPointer& vertices, std::size_t count, SelectionIntersection& best ) override {
		if ( count == 0 ) {
			return;
		}
		Vector4 clipped[9];
		for ( VertexPointer::iterator i = vertices.begin(), end = i + count; i != end; i += 2 )
		{
			BestPoint(
			    matrix4_clip_line(
			        m_local2view,
			        reinterpret_cast<const Vector3&>( ( *i ) ),
			        reinterpret_cast<const Vector3&>( ( *( i + 1 ) ) ),
			        clipped
			    ),
			    clipped,
			    best,
			    m_cull
			);
		}
	}
	void TestTriangles( const VertexPointer& vertices, const IndexPointer& indices, SelectionIntersection& best ) override {
		Vector4 clipped[9];
		for ( IndexPointer::iterator i( indices.begin() ); i != indices.end(); i += 3 )
		{
			BestPoint(
			    matrix4_clip_triangle(
			        m_local2view,
			        reinterpret_cast<const Vector3&>( vertices[*i] ),
			        reinterpret_cast<const Vector3&>( vertices[*( i + 1 )] ),
			        reinterpret_cast<const Vector3&>( vertices[*( i + 2 )] ),
			        clipped
			    ),
			    clipped,
			    best,
			    m_cull
			);
		}
	}
	void TestQuads( const VertexPointer& vertices, const IndexPointer& indices, SelectionIntersection& best ) override {
		Vector4 clipped[9];
		for ( IndexPointer::iterator i( indices.begin() ); i != indices.end(); i += 4 )
		{
			BestPoint(
			    matrix4_clip_triangle(
			        m_local2view,
			        reinterpret_cast<const Vector3&>( vertices[*i] ),
			        reinterpret_cast<const Vector3&>( vertices[*( i + 1 )] ),
			        reinterpret_cast<const Vector3&>( vertices[*( i + 3 )] ),
			        clipped
			    ),
			    clipped,
			    best,
			    m_cull
			);
			BestPoint(
			    matrix4_clip_triangle(
			        m_local2view,
			        reinterpret_cast<const Vector3&>( vertices[*( i + 1 )] ),
			        reinterpret_cast<const Vector3&>( vertices[*( i + 2 )] ),
			        reinterpret_cast<const Vector3&>( vertices[*( i + 3 )] ),
			        clipped
			    ),
			    clipped,
			    best,
			    m_cull
			);
		}
	}
	void TestQuadStrip( const VertexPointer& vertices, const IndexPointer& indices, SelectionIntersection& best ) override {
		Vector4 clipped[9];
		for ( IndexPointer::iterator i( indices.begin() ); i + 2 != indices.end(); i += 2 )
		{
			BestPoint(
			    matrix4_clip_triangle(
			        m_local2view,
			        reinterpret_cast<const Vector3&>( vertices[*i] ),
			        reinterpret_cast<const Vector3&>( vertices[*( i + 1 )] ),
			        reinterpret_cast<const Vector3&>( vertices[*( i + 2 )] ),
			        clipped
			    ),
			    clipped,
			    best,
			    m_cull
			);
			BestPoint(
			    matrix4_clip_triangle(
			        m_local2view,
			        reinterpret_cast<const Vector3&>( vertices[*( i + 2 )] ),
			        reinterpret_cast<const Vector3&>( vertices[*( i + 1 )] ),
			        reinterpret_cast<const Vector3&>( vertices[*( i + 3 )] ),
			        clipped
			    ),
			    clipped,
			    best,
			    m_cull
			);
		}
	}
};



typedef std::multimap<SelectionIntersection, Selectable*> SelectableSortedSet;

class SelectionPool : public Selector
{
	SelectableSortedSet m_pool;
	SelectionIntersection m_intersection;
	Selectable* m_selectable;

public:
	void pushSelectable( Selectable& selectable ) override {
		m_intersection = SelectionIntersection();
		m_selectable = &selectable;
	}
	void popSelectable() override {
		addSelectable( m_intersection, m_selectable );
		m_intersection = SelectionIntersection();
	}
	void addIntersection( const SelectionIntersection& intersection ) override {
		assign_if_closer( m_intersection, intersection );
	}
	void addSelectable( const SelectionIntersection& intersection, Selectable* selectable ){
		if ( intersection.valid() ) {
			m_pool.insert( SelectableSortedSet::value_type( intersection, selectable ) );
		}
	}

	typedef SelectableSortedSet::iterator iterator;

	iterator begin(){
		return m_pool.begin();
	}
	iterator end(){
		return m_pool.end();
	}

	bool failed(){
		return m_pool.empty();
	}
};


class ManipulatorSelectionChangeable
{
	const Selectable* m_selectable_prev_ptr = nullptr;
public:
	void selectionChange( const Selectable *se ){
		if( m_selectable_prev_ptr != se ){
			m_selectable_prev_ptr = se;
			SceneChangeNotify();
		}
	}
	void selectionChange( SelectionPool& selector ){
		Selectable *se = nullptr;
		if ( !selector.failed() ) {
			se = selector.begin()->second;
			se->setSelected( true );
		}
		selectionChange( se );
	}
};


const Colour4b g_colour_sphere( 0, 0, 0, 255 );
const Colour4b g_colour_screen( 0, 255, 255, 255 );
const Colour4b g_colour_selected( 255, 255, 0, 255 );

inline const Colour4b& colourSelected( const Colour4b& colour, bool selected ){
	return ( selected ) ? g_colour_selected : colour;
}

template<typename remap_policy>
inline void draw_semicircle( const std::size_t segments, const float radius, PointVertex* vertices, remap_policy remap ){
	const double increment = c_pi / double( segments << 2 );

	std::size_t count = 0;
	float x = radius;
	float y = 0;
	remap_policy::set( vertices[segments << 2].vertex, -radius, 0, 0 );
	while ( count < segments )
	{
		PointVertex* i = vertices + count;
		PointVertex* j = vertices + ( ( segments << 1 ) - ( count + 1 ) );

		PointVertex* k = i + ( segments << 1 );
		PointVertex* l = j + ( segments << 1 );

#if 0
		PointVertex* m = i + ( segments << 2 );
		PointVertex* n = j + ( segments << 2 );
		PointVertex* o = k + ( segments << 2 );
		PointVertex* p = l + ( segments << 2 );
#endif

		remap_policy::set( i->vertex, x,-y, 0 );
		remap_policy::set( k->vertex,-y,-x, 0 );
#if 0
		remap_policy::set( m->vertex,-x, y, 0 );
		remap_policy::set( o->vertex, y, x, 0 );
#endif

		++count;

		{
			const double theta = increment * count;
			x = static_cast<float>( radius * cos( theta ) );
			y = static_cast<float>( radius * sin( theta ) );
		}

		remap_policy::set( j->vertex, y,-x, 0 );
		remap_policy::set( l->vertex,-x,-y, 0 );
#if 0
		remap_policy::set( n->vertex,-y, x, 0 );
		remap_policy::set( p->vertex, x, y, 0 );
#endif
	}
}

class Manipulator
{
public:
	virtual Manipulatable* GetManipulatable() = 0;
	virtual void testSelect( const View& view, const Matrix4& pivot2world ) = 0;
	virtual void render( Renderer& renderer, const VolumeTest& volume, const Matrix4& pivot2world ){
	}
	virtual void setSelected( bool select ) = 0;
	virtual bool isSelected() const = 0;
};


inline Vector3 normalised_safe( const Vector3& self ){
	if ( vector3_equal( self, g_vector3_identity ) ) {
		return g_vector3_identity;
	}
	return vector3_normalised( self );
}


class RotateManipulator : public Manipulator, public ManipulatorSelectionChangeable
{
	struct RenderableCircle : public OpenGLRenderable
	{
		Array<PointVertex> m_vertices;

		RenderableCircle( std::size_t size ) : m_vertices( size ){
		}
		void render( RenderStateFlags state ) const override {
			gl().glColorPointer( 4, GL_UNSIGNED_BYTE, sizeof( PointVertex ), &m_vertices.data()->colour );
			gl().glVertexPointer( 3, GL_FLOAT, sizeof( PointVertex ), &m_vertices.data()->vertex );
			gl().glDrawArrays( GL_LINE_LOOP, 0, GLsizei( m_vertices.size() ) );
		}
		void setColour( const Colour4b& colour ){
			for ( Array<PointVertex>::iterator i = m_vertices.begin(); i != m_vertices.end(); ++i )
			{
				( *i ).colour = colour;
			}
		}
	};

	struct RenderableSemiCircle : public OpenGLRenderable
	{
		Array<PointVertex> m_vertices;

		RenderableSemiCircle( std::size_t size ) : m_vertices( size ){
		}
		void render( RenderStateFlags state ) const override {
			gl().glColorPointer( 4, GL_UNSIGNED_BYTE, sizeof( PointVertex ), &m_vertices.data()->colour );
			gl().glVertexPointer( 3, GL_FLOAT, sizeof( PointVertex ), &m_vertices.data()->vertex );
			gl().glDrawArrays( GL_LINE_STRIP, 0, GLsizei( m_vertices.size() ) );
		}
		void setColour( const Colour4b& colour ){
			for ( Array<PointVertex>::iterator i = m_vertices.begin(); i != m_vertices.end(); ++i )
			{
				( *i ).colour = colour;
			}
		}
	};

	RotateFree m_free;
	RotateAxis m_axis;
	Vector3 m_axis_screen;
	RenderableSemiCircle m_circle_x;
	RenderableSemiCircle m_circle_y;
	RenderableSemiCircle m_circle_z;
	RenderableCircle m_circle_screen;
	RenderableCircle m_circle_sphere;
	SelectableBool m_selectable_x;
	SelectableBool m_selectable_y;
	SelectableBool m_selectable_z;
	SelectableBool m_selectable_screen;
	SelectableBool m_selectable_sphere;
	Pivot2World m_pivot;
	Matrix4 m_local2world_x;
	Matrix4 m_local2world_y;
	Matrix4 m_local2world_z;
	bool m_circle_x_visible;
	bool m_circle_y_visible;
	bool m_circle_z_visible;
public:
	static Shader* m_state_outer;

	RotateManipulator( Rotatable& rotatable, std::size_t segments, float radius ) :
		m_free( rotatable ),
		m_axis( rotatable ),
		m_circle_x( ( segments << 2 ) + 1 ),
		m_circle_y( ( segments << 2 ) + 1 ),
		m_circle_z( ( segments << 2 ) + 1 ),
		m_circle_screen( segments << 3 ),
		m_circle_sphere( segments << 3 ){
		draw_semicircle( segments, radius, m_circle_x.m_vertices.data(), RemapYZX() );
		draw_semicircle( segments, radius, m_circle_y.m_vertices.data(), RemapZXY() );
		draw_semicircle( segments, radius, m_circle_z.m_vertices.data(), RemapXYZ() );

		draw_circle( segments, radius * 1.15f, m_circle_screen.m_vertices.data(), RemapXYZ() );
		draw_circle( segments, radius, m_circle_sphere.m_vertices.data(), RemapXYZ() );
	}


	void UpdateColours(){
		m_circle_x.setColour( colourSelected( g_colour_x, m_selectable_x.isSelected() ) );
		m_circle_y.setColour( colourSelected( g_colour_y, m_selectable_y.isSelected() ) );
		m_circle_z.setColour( colourSelected( g_colour_z, m_selectable_z.isSelected() ) );
		m_circle_screen.setColour( colourSelected( g_colour_screen, m_selectable_screen.isSelected() ) );
		m_circle_sphere.setColour( colourSelected( g_colour_sphere, false ) );
	}

	void updateCircleTransforms(){
		Vector3 localViewpoint( matrix4_transformed_direction( matrix4_transposed( m_pivot.m_worldSpace ), m_pivot.m_viewpointSpace.z().vec3() ) );

		m_circle_x_visible = !vector3_equal_epsilon( g_vector3_axis_x, localViewpoint, 1e-6f );
		if ( m_circle_x_visible ) {
			m_local2world_x = g_matrix4_identity;
			m_local2world_x.y().vec3() = normalised_safe(
			            vector3_cross( g_vector3_axis_x, localViewpoint )
			        );
			m_local2world_x.z().vec3() = normalised_safe(
			            vector3_cross( m_local2world_x.x().vec3(), m_local2world_x.y().vec3() )
			        );
			matrix4_premultiply_by_matrix4( m_local2world_x, m_pivot.m_worldSpace );
		}

		m_circle_y_visible = !vector3_equal_epsilon( g_vector3_axis_y, localViewpoint, 1e-6f );
		if ( m_circle_y_visible ) {
			m_local2world_y = g_matrix4_identity;
			m_local2world_y.z().vec3() = normalised_safe(
			            vector3_cross( g_vector3_axis_y, localViewpoint )
			        );
			m_local2world_y.x().vec3() = normalised_safe(
			            vector3_cross( m_local2world_y.y().vec3(), m_local2world_y.z().vec3() )
			        );
			matrix4_premultiply_by_matrix4( m_local2world_y, m_pivot.m_worldSpace );
		}

		m_circle_z_visible = !vector3_equal_epsilon( g_vector3_axis_z, localViewpoint, 1e-6f );
		if ( m_circle_z_visible ) {
			m_local2world_z = g_matrix4_identity;
			m_local2world_z.x().vec3() = normalised_safe(
			            vector3_cross( g_vector3_axis_z, localViewpoint )
			        );
			m_local2world_z.y().vec3() = normalised_safe(
			            vector3_cross( m_local2world_z.z().vec3(), m_local2world_z.x().vec3() )
			        );
			matrix4_premultiply_by_matrix4( m_local2world_z, m_pivot.m_worldSpace );
		}
	}

	void render( Renderer& renderer, const VolumeTest& volume, const Matrix4& pivot2world ) override {
		m_pivot.update( pivot2world, volume.GetModelview(), volume.GetProjection(), volume.GetViewport() );
		updateCircleTransforms();

		// temp hack
		UpdateColours();

		renderer.SetState( m_state_outer, Renderer::eWireframeOnly );
		renderer.SetState( m_state_outer, Renderer::eFullMaterials );

		renderer.addRenderable( m_circle_screen, m_pivot.m_viewpointSpace );
		renderer.addRenderable( m_circle_sphere, m_pivot.m_viewpointSpace );

		if ( m_circle_x_visible ) {
			renderer.addRenderable( m_circle_x, m_local2world_x );
		}
		if ( m_circle_y_visible ) {
			renderer.addRenderable( m_circle_y, m_local2world_y );
		}
		if ( m_circle_z_visible ) {
			renderer.addRenderable( m_circle_z, m_local2world_z );
		}
	}
	void testSelect( const View& view, const Matrix4& pivot2world ) override {
		if( g_modifiers != c_modifierNone )
			return selectionChange( nullptr );

		m_pivot.update( pivot2world, view.GetModelview(), view.GetProjection(), view.GetViewport() );
		updateCircleTransforms();

		SelectionPool selector;

		{
			{
				const Matrix4 local2view( matrix4_multiplied_by_matrix4( view.GetViewMatrix(), m_local2world_x ) );

#if defined( DEBUG_SELECTION )
				g_render_clipped.construct( view.GetViewMatrix() );
#endif

				SelectionIntersection best;
				LineStrip_BestPoint( local2view, m_circle_x.m_vertices.data(), m_circle_x.m_vertices.size(), best );
				selector.addSelectable( best, &m_selectable_x );
			}

			{
				const Matrix4 local2view( matrix4_multiplied_by_matrix4( view.GetViewMatrix(), m_local2world_y ) );

#if defined( DEBUG_SELECTION )
				g_render_clipped.construct( view.GetViewMatrix() );
#endif

				SelectionIntersection best;
				LineStrip_BestPoint( local2view, m_circle_y.m_vertices.data(), m_circle_y.m_vertices.size(), best );
				selector.addSelectable( best, &m_selectable_y );
			}

			{
				const Matrix4 local2view( matrix4_multiplied_by_matrix4( view.GetViewMatrix(), m_local2world_z ) );

#if defined( DEBUG_SELECTION )
				g_render_clipped.construct( view.GetViewMatrix() );
#endif

				SelectionIntersection best;
				LineStrip_BestPoint( local2view, m_circle_z.m_vertices.data(), m_circle_z.m_vertices.size(), best );
				selector.addSelectable( best, &m_selectable_z );
			}
		}

		{
			const Matrix4 local2view( matrix4_multiplied_by_matrix4( view.GetViewMatrix(), m_pivot.m_viewpointSpace ) );

			{
				SelectionIntersection best;
				LineLoop_BestPoint( local2view, m_circle_screen.m_vertices.data(), m_circle_screen.m_vertices.size(), best );
				selector.addSelectable( best, &m_selectable_screen );
			}

//		{
//			SelectionIntersection best;
//			Circle_BestPoint( local2view, eClipCullCW, m_circle_sphere.m_vertices.data(), m_circle_sphere.m_vertices.size(), best );
//			selector.addSelectable( best, &m_selectable_sphere );
//		}
		}

		m_axis_screen = m_pivot.m_axis_screen;

		if ( selector.failed() )
			selector.addSelectable( SelectionIntersection( 0, 0 ), &m_selectable_sphere );

		selectionChange( selector );
	}

	Manipulatable* GetManipulatable() override {
		if ( m_selectable_x.isSelected() ) {
			m_axis.SetAxis( g_vector3_axis_x );
			return &m_axis;
		}
		else if ( m_selectable_y.isSelected() ) {
			m_axis.SetAxis( g_vector3_axis_y );
			return &m_axis;
		}
		else if ( m_selectable_z.isSelected() ) {
			m_axis.SetAxis( g_vector3_axis_z );
			return &m_axis;
		}
		else if ( m_selectable_screen.isSelected() ) {
			m_axis.SetAxis( m_axis_screen );
			return &m_axis;
		}
		else{
			return &m_free;
		}
	}

	void setSelected( bool select ) override {
		m_selectable_x.setSelected( select );
		m_selectable_y.setSelected( select );
		m_selectable_z.setSelected( select );
		m_selectable_screen.setSelected( select );
		m_selectable_sphere.setSelected( select );
	}
	bool isSelected() const override {
		return m_selectable_x.isSelected()
		    || m_selectable_y.isSelected()
		    || m_selectable_z.isSelected()
		    || m_selectable_screen.isSelected()
		    || m_selectable_sphere.isSelected();
	}
};

Shader* RotateManipulator::m_state_outer;


const float arrowhead_length = 16;
const float arrowhead_radius = 4;

inline void draw_arrowline( const float length, PointVertex* line, const std::size_t axis ){
	( *line++ ).vertex = vertex3f_identity;
	( *line ).vertex = vertex3f_identity;
	vertex3f_to_array( ( *line ).vertex )[axis] = length - arrowhead_length;
}

template<typename VertexRemap, typename NormalRemap>
inline void draw_arrowhead( const std::size_t segments, const float length, FlatShadedVertex* vertices, VertexRemap, NormalRemap ){
	std::size_t head_tris = ( segments << 3 );
	const double head_segment = c_2pi / head_tris;
	for ( std::size_t i = 0; i < head_tris; ++i )
	{
		{
			FlatShadedVertex& point = vertices[i * 6 + 0];
			VertexRemap::x( point.vertex ) = length - arrowhead_length;
			VertexRemap::y( point.vertex ) = arrowhead_radius * static_cast<float>( cos( i * head_segment ) );
			VertexRemap::z( point.vertex ) = arrowhead_radius * static_cast<float>( sin( i * head_segment ) );
			NormalRemap::x( point.normal ) = arrowhead_radius / arrowhead_length;
			NormalRemap::y( point.normal ) = static_cast<float>( cos( i * head_segment ) );
			NormalRemap::z( point.normal ) = static_cast<float>( sin( i * head_segment ) );
		}
		{
			FlatShadedVertex& point = vertices[i * 6 + 1];
			VertexRemap::x( point.vertex ) = length;
			VertexRemap::y( point.vertex ) = 0;
			VertexRemap::z( point.vertex ) = 0;
			NormalRemap::x( point.normal ) = arrowhead_radius / arrowhead_length;
			NormalRemap::y( point.normal ) = static_cast<float>( cos( ( i + 0.5 ) * head_segment ) );
			NormalRemap::z( point.normal ) = static_cast<float>( sin( ( i + 0.5 ) * head_segment ) );
		}
		{
			FlatShadedVertex& point = vertices[i * 6 + 2];
			VertexRemap::x( point.vertex ) = length - arrowhead_length;
			VertexRemap::y( point.vertex ) = arrowhead_radius * static_cast<float>( cos( ( i + 1 ) * head_segment ) );
			VertexRemap::z( point.vertex ) = arrowhead_radius * static_cast<float>( sin( ( i + 1 ) * head_segment ) );
			NormalRemap::x( point.normal ) = arrowhead_radius / arrowhead_length;
			NormalRemap::y( point.normal ) = static_cast<float>( cos( ( i + 1 ) * head_segment ) );
			NormalRemap::z( point.normal ) = static_cast<float>( sin( ( i + 1 ) * head_segment ) );
		}

		{
			FlatShadedVertex& point = vertices[i * 6 + 3];
			VertexRemap::x( point.vertex ) = length - arrowhead_length;
			VertexRemap::y( point.vertex ) = 0;
			VertexRemap::z( point.vertex ) = 0;
			NormalRemap::x( point.normal ) = -1;
			NormalRemap::y( point.normal ) = 0;
			NormalRemap::z( point.normal ) = 0;
		}
		{
			FlatShadedVertex& point = vertices[i * 6 + 4];
			VertexRemap::x( point.vertex ) = length - arrowhead_length;
			VertexRemap::y( point.vertex ) = arrowhead_radius * static_cast<float>( cos( i * head_segment ) );
			VertexRemap::z( point.vertex ) = arrowhead_radius * static_cast<float>( sin( i * head_segment ) );
			NormalRemap::x( point.normal ) = -1;
			NormalRemap::y( point.normal ) = 0;
			NormalRemap::z( point.normal ) = 0;
		}
		{
			FlatShadedVertex& point = vertices[i * 6 + 5];
			VertexRemap::x( point.vertex ) = length - arrowhead_length;
			VertexRemap::y( point.vertex ) = arrowhead_radius * static_cast<float>( cos( ( i + 1 ) * head_segment ) );
			VertexRemap::z( point.vertex ) = arrowhead_radius * static_cast<float>( sin( ( i + 1 ) * head_segment ) );
			NormalRemap::x( point.normal ) = -1;
			NormalRemap::y( point.normal ) = 0;
			NormalRemap::z( point.normal ) = 0;
		}
	}
}

template<typename Triple>
class TripleRemapXYZ
{
public:
	static float& x( Triple& triple ){
		return triple.x();
	}
	static float& y( Triple& triple ){
		return triple.y();
	}
	static float& z( Triple& triple ){
		return triple.z();
	}
};

template<typename Triple>
class TripleRemapYZX
{
public:
	static float& x( Triple& triple ){
		return triple.y();
	}
	static float& y( Triple& triple ){
		return triple.z();
	}
	static float& z( Triple& triple ){
		return triple.x();
	}
};

template<typename Triple>
class TripleRemapZXY
{
public:
	static float& x( Triple& triple ){
		return triple.z();
	}
	static float& y( Triple& triple ){
		return triple.x();
	}
	static float& z( Triple& triple ){
		return triple.y();
	}
};



class TranslateManipulator : public Manipulator, public ManipulatorSelectionChangeable
{
	struct RenderableArrowLine : public OpenGLRenderable
	{
		PointVertex m_line[2];

		RenderableArrowLine(){
		}
		void render( RenderStateFlags state ) const override {
			gl().glColorPointer( 4, GL_UNSIGNED_BYTE, sizeof( PointVertex ), &m_line[0].colour );
			gl().glVertexPointer( 3, GL_FLOAT, sizeof( PointVertex ), &m_line[0].vertex );
			gl().glDrawArrays( GL_LINES, 0, 2 );
		}
		void setColour( const Colour4b& colour ){
			m_line[0].colour = colour;
			m_line[1].colour = colour;
		}
	};
	struct RenderableArrowHead : public OpenGLRenderable
	{
		Array<FlatShadedVertex> m_vertices;

		RenderableArrowHead( std::size_t size )
			: m_vertices( size ){
		}
		void render( RenderStateFlags state ) const override {
			gl().glColorPointer( 4, GL_UNSIGNED_BYTE, sizeof( FlatShadedVertex ), &m_vertices.data()->colour );
			gl().glVertexPointer( 3, GL_FLOAT, sizeof( FlatShadedVertex ), &m_vertices.data()->vertex );
			gl().glNormalPointer( GL_FLOAT, sizeof( FlatShadedVertex ), &m_vertices.data()->normal );
			gl().glDrawArrays( GL_TRIANGLES, 0, GLsizei( m_vertices.size() ) );
		}
		void setColour( const Colour4b& colour ){
			for ( Array<FlatShadedVertex>::iterator i = m_vertices.begin(); i != m_vertices.end(); ++i )
			{
				( *i ).colour = colour;
			}
		}
	};
	struct RenderableQuad : public OpenGLRenderable
	{
		PointVertex m_quad[4];
		void render( RenderStateFlags state ) const override {
			gl().glColorPointer( 4, GL_UNSIGNED_BYTE, sizeof( PointVertex ), &m_quad[0].colour );
			gl().glVertexPointer( 3, GL_FLOAT, sizeof( PointVertex ), &m_quad[0].vertex );
			gl().glDrawArrays( GL_LINE_LOOP, 0, 4 );
		}
		void setColour( const Colour4b& colour ){
			m_quad[0].colour = colour;
			m_quad[1].colour = colour;
			m_quad[2].colour = colour;
			m_quad[3].colour = colour;
		}
	};

	TranslateFree m_free;
	TranslateAxis m_axis;
	RenderableArrowLine m_arrow_x;
	RenderableArrowLine m_arrow_y;
	RenderableArrowLine m_arrow_z;
	RenderableArrowHead m_arrow_head_x;
	RenderableArrowHead m_arrow_head_y;
	RenderableArrowHead m_arrow_head_z;
	RenderableQuad m_quad_screen;
	SelectableBool m_selectable_x;
	SelectableBool m_selectable_y;
	SelectableBool m_selectable_z;
	SelectableBool m_selectable_screen;
	Pivot2World m_pivot;
public:
	static Shader* m_state_wire;
	static Shader* m_state_fill;

	TranslateManipulator( Translatable& translatable, std::size_t segments, float length ) :
		m_free( translatable ),
		m_axis( translatable ),
		m_arrow_head_x( 3 * 2 * ( segments << 3 ) ),
		m_arrow_head_y( 3 * 2 * ( segments << 3 ) ),
		m_arrow_head_z( 3 * 2 * ( segments << 3 ) ){
		draw_arrowline( length, m_arrow_x.m_line, 0 );
		draw_arrowhead( segments, length, m_arrow_head_x.m_vertices.data(), TripleRemapXYZ<Vertex3f>(), TripleRemapXYZ<Normal3f>() );
		draw_arrowline( length, m_arrow_y.m_line, 1 );
		draw_arrowhead( segments, length, m_arrow_head_y.m_vertices.data(), TripleRemapYZX<Vertex3f>(), TripleRemapYZX<Normal3f>() );
		draw_arrowline( length, m_arrow_z.m_line, 2 );
		draw_arrowhead( segments, length, m_arrow_head_z.m_vertices.data(), TripleRemapZXY<Vertex3f>(), TripleRemapZXY<Normal3f>() );

		draw_quad( 16, m_quad_screen.m_quad );
	}

	void UpdateColours(){
		m_arrow_x.setColour( colourSelected( g_colour_x, m_selectable_x.isSelected() ) );
		m_arrow_head_x.setColour( colourSelected( g_colour_x, m_selectable_x.isSelected() ) );
		m_arrow_y.setColour( colourSelected( g_colour_y, m_selectable_y.isSelected() ) );
		m_arrow_head_y.setColour( colourSelected( g_colour_y, m_selectable_y.isSelected() ) );
		m_arrow_z.setColour( colourSelected( g_colour_z, m_selectable_z.isSelected() ) );
		m_arrow_head_z.setColour( colourSelected( g_colour_z, m_selectable_z.isSelected() ) );
		m_quad_screen.setColour( colourSelected( g_colour_screen, m_selectable_screen.isSelected() ) );
	}

	bool manipulator_show_axis( const Pivot2World& pivot, const Vector3& axis ){
		return fabs( vector3_dot( pivot.m_axis_screen, axis ) ) < 0.95;
	}

	void render( Renderer& renderer, const VolumeTest& volume, const Matrix4& pivot2world ) override {
		m_pivot.update( pivot2world, volume.GetModelview(), volume.GetProjection(), volume.GetViewport() );

		// temp hack
		UpdateColours();

		Vector3 x = vector3_normalised( m_pivot.m_worldSpace.x().vec3() );
		bool show_x = manipulator_show_axis( m_pivot, x );

		Vector3 y = vector3_normalised( m_pivot.m_worldSpace.y().vec3() );
		bool show_y = manipulator_show_axis( m_pivot, y );

		Vector3 z = vector3_normalised( m_pivot.m_worldSpace.z().vec3() );
		bool show_z = manipulator_show_axis( m_pivot, z );

		renderer.SetState( m_state_wire, Renderer::eWireframeOnly );
		renderer.SetState( m_state_wire, Renderer::eFullMaterials );

		if ( show_x ) {
			renderer.addRenderable( m_arrow_x, m_pivot.m_worldSpace );
		}
		if ( show_y ) {
			renderer.addRenderable( m_arrow_y, m_pivot.m_worldSpace );
		}
		if ( show_z ) {
			renderer.addRenderable( m_arrow_z, m_pivot.m_worldSpace );
		}

		renderer.addRenderable( m_quad_screen, m_pivot.m_viewplaneSpace );

		renderer.SetState( m_state_fill, Renderer::eWireframeOnly );
		renderer.SetState( m_state_fill, Renderer::eFullMaterials );

		if ( show_x ) {
			renderer.addRenderable( m_arrow_head_x, m_pivot.m_worldSpace );
		}
		if ( show_y ) {
			renderer.addRenderable( m_arrow_head_y, m_pivot.m_worldSpace );
		}
		if ( show_z ) {
			renderer.addRenderable( m_arrow_head_z, m_pivot.m_worldSpace );
		}
	}
	void testSelect( const View& view, const Matrix4& pivot2world ) override {
		if( g_modifiers != c_modifierNone )
			return selectionChange( nullptr );

		m_pivot.update( pivot2world, view.GetModelview(), view.GetProjection(), view.GetViewport() );

		SelectionPool selector;

		Vector3 x = vector3_normalised( m_pivot.m_worldSpace.x().vec3() );
		bool show_x = manipulator_show_axis( m_pivot, x );

		Vector3 y = vector3_normalised( m_pivot.m_worldSpace.y().vec3() );
		bool show_y = manipulator_show_axis( m_pivot, y );

		Vector3 z = vector3_normalised( m_pivot.m_worldSpace.z().vec3() );
		bool show_z = manipulator_show_axis( m_pivot, z );

		{
			const Matrix4 local2view( matrix4_multiplied_by_matrix4( view.GetViewMatrix(), m_pivot.m_viewpointSpace ) );

			{
				SelectionIntersection best;
				Quad_BestPoint( local2view, eClipCullCW, m_quad_screen.m_quad, best );
				if ( best.valid() ) {
					best = SelectionIntersection( 0, 0 );
					selector.addSelectable( best, &m_selectable_screen );
				}
			}
		}

		{
			const Matrix4 local2view( matrix4_multiplied_by_matrix4( view.GetViewMatrix(), m_pivot.m_worldSpace ) );

#if defined( DEBUG_SELECTION )
			g_render_clipped.construct( view.GetViewMatrix() );
#endif

			if ( show_x ) {
				SelectionIntersection best;
				Line_BestPoint( local2view, m_arrow_x.m_line, best );
				Triangles_BestPoint( local2view, eClipCullCW, m_arrow_head_x.m_vertices.begin(), m_arrow_head_x.m_vertices.end(), best );
				selector.addSelectable( best, &m_selectable_x );
			}

			if ( show_y ) {
				SelectionIntersection best;
				Line_BestPoint( local2view, m_arrow_y.m_line, best );
				Triangles_BestPoint( local2view, eClipCullCW, m_arrow_head_y.m_vertices.begin(), m_arrow_head_y.m_vertices.end(), best );
				selector.addSelectable( best, &m_selectable_y );
			}

			if ( show_z ) {
				SelectionIntersection best;
				Line_BestPoint( local2view, m_arrow_z.m_line, best );
				Triangles_BestPoint( local2view, eClipCullCW, m_arrow_head_z.m_vertices.begin(), m_arrow_head_z.m_vertices.end(), best );
				selector.addSelectable( best, &m_selectable_z );
			}
		}

		selectionChange( selector );
	}

	Manipulatable* GetManipulatable() override {
		if ( m_selectable_x.isSelected() ) {
			m_axis.SetAxis( g_vector3_axis_x );
			return &m_axis;
		}
		else if ( m_selectable_y.isSelected() ) {
			m_axis.SetAxis( g_vector3_axis_y );
			return &m_axis;
		}
		else if ( m_selectable_z.isSelected() ) {
			m_axis.SetAxis( g_vector3_axis_z );
			return &m_axis;
		}
		else
		{
			return &m_free;
		}
	}

	void setSelected( bool select ) override {
		m_selectable_x.setSelected( select );
		m_selectable_y.setSelected( select );
		m_selectable_z.setSelected( select );
		m_selectable_screen.setSelected( select );
	}
	bool isSelected() const override {
		return m_selectable_x.isSelected()
		    || m_selectable_y.isSelected()
		    || m_selectable_z.isSelected()
		    || m_selectable_screen.isSelected();
	}
};

Shader* TranslateManipulator::m_state_wire;
Shader* TranslateManipulator::m_state_fill;

class ScaleManipulator : public Manipulator, public ManipulatorSelectionChangeable
{
	struct RenderableArrow : public OpenGLRenderable
	{
		PointVertex m_line[2];

		void render( RenderStateFlags state ) const override {
			gl().glColorPointer( 4, GL_UNSIGNED_BYTE, sizeof( PointVertex ), &m_line[0].colour );
			gl().glVertexPointer( 3, GL_FLOAT, sizeof( PointVertex ), &m_line[0].vertex );
			gl().glDrawArrays( GL_LINES, 0, 2 );
		}
		void setColour( const Colour4b& colour ){
			m_line[0].colour = colour;
			m_line[1].colour = colour;
		}
	};
	struct RenderableQuad : public OpenGLRenderable
	{
		PointVertex m_quad[4];
		void render( RenderStateFlags state ) const override {
			gl().glColorPointer( 4, GL_UNSIGNED_BYTE, sizeof( PointVertex ), &m_quad[0].colour );
			gl().glVertexPointer( 3, GL_FLOAT, sizeof( PointVertex ), &m_quad[0].vertex );
			gl().glDrawArrays( GL_QUADS, 0, 4 );
		}
		void setColour( const Colour4b& colour ){
			m_quad[0].colour = colour;
			m_quad[1].colour = colour;
			m_quad[2].colour = colour;
			m_quad[3].colour = colour;
		}
	};

	ScaleFree m_free;
	ScaleAxis m_axis;
	RenderableArrow m_arrow_x;
	RenderableArrow m_arrow_y;
	RenderableArrow m_arrow_z;
	RenderableQuad m_quad_screen;
	SelectableBool m_selectable_x;
	SelectableBool m_selectable_y;
	SelectableBool m_selectable_z;
	SelectableBool m_selectable_screen;
	Pivot2World m_pivot;
public:
	ScaleManipulator( Scalable& scalable, std::size_t segments, float length ) :
		m_free( scalable ),
		m_axis( scalable ){
		draw_arrowline( length, m_arrow_x.m_line, 0 );
		draw_arrowline( length, m_arrow_y.m_line, 1 );
		draw_arrowline( length, m_arrow_z.m_line, 2 );

		draw_quad( 16, m_quad_screen.m_quad );
	}

	void UpdateColours(){
		m_arrow_x.setColour( colourSelected( g_colour_x, m_selectable_x.isSelected() ) );
		m_arrow_y.setColour( colourSelected( g_colour_y, m_selectable_y.isSelected() ) );
		m_arrow_z.setColour( colourSelected( g_colour_z, m_selectable_z.isSelected() ) );
		m_quad_screen.setColour( colourSelected( g_colour_screen, m_selectable_screen.isSelected() ) );
	}

	void render( Renderer& renderer, const VolumeTest& volume, const Matrix4& pivot2world ) override {
		m_pivot.update( pivot2world, volume.GetModelview(), volume.GetProjection(), volume.GetViewport() );

		// temp hack
		UpdateColours();

		renderer.addRenderable( m_arrow_x, m_pivot.m_worldSpace );
		renderer.addRenderable( m_arrow_y, m_pivot.m_worldSpace );
		renderer.addRenderable( m_arrow_z, m_pivot.m_worldSpace );

		renderer.addRenderable( m_quad_screen, m_pivot.m_viewpointSpace );
	}
	void testSelect( const View& view, const Matrix4& pivot2world ) override {
		if( g_modifiers != c_modifierNone )
			return selectionChange( nullptr );

		m_pivot.update( pivot2world, view.GetModelview(), view.GetProjection(), view.GetViewport() );

		SelectionPool selector;

		{
			const Matrix4 local2view( matrix4_multiplied_by_matrix4( view.GetViewMatrix(), m_pivot.m_worldSpace ) );

#if defined( DEBUG_SELECTION )
			g_render_clipped.construct( view.GetViewMatrix() );
#endif

			{
				SelectionIntersection best;
				Line_BestPoint( local2view, m_arrow_x.m_line, best );
				selector.addSelectable( best, &m_selectable_x );
			}

			{
				SelectionIntersection best;
				Line_BestPoint( local2view, m_arrow_y.m_line, best );
				selector.addSelectable( best, &m_selectable_y );
			}

			{
				SelectionIntersection best;
				Line_BestPoint( local2view, m_arrow_z.m_line, best );
				selector.addSelectable( best, &m_selectable_z );
			}
		}

		{
			const Matrix4 local2view( matrix4_multiplied_by_matrix4( view.GetViewMatrix(), m_pivot.m_viewpointSpace ) );

			{
				SelectionIntersection best;
				Quad_BestPoint( local2view, eClipCullCW, m_quad_screen.m_quad, best );
				selector.addSelectable( best, &m_selectable_screen );
			}
		}

		selectionChange( selector );
	}

	Manipulatable* GetManipulatable() override {
		if ( m_selectable_x.isSelected() ) {
			m_axis.SetAxis( g_vector3_axis_x );
			return &m_axis;
		}
		else if ( m_selectable_y.isSelected() ) {
			m_axis.SetAxis( g_vector3_axis_y );
			return &m_axis;
		}
		else if ( m_selectable_z.isSelected() ) {
			m_axis.SetAxis( g_vector3_axis_z );
			return &m_axis;
		}
		else{
			m_free.SetAxes( g_vector3_identity, g_vector3_identity );
			return &m_free;
		}
	}

	void setSelected( bool select ) override {
		m_selectable_x.setSelected( select );
		m_selectable_y.setSelected( select );
		m_selectable_z.setSelected( select );
		m_selectable_screen.setSelected( select );
	}
	bool isSelected() const override {
		return m_selectable_x.isSelected()
		    || m_selectable_y.isSelected()
		    || m_selectable_z.isSelected()
		    || m_selectable_screen.isSelected();
	}
};


#include "dragplanes.h"

class SkewManipulator : public Manipulator, public ManipulatorSelectionChangeable
{
	struct RenderableLine : public OpenGLRenderable {
		PointVertex m_line[2];

		RenderableLine() {
		}
		void render( RenderStateFlags state ) const override {
			gl().glColorPointer( 4, GL_UNSIGNED_BYTE, sizeof( PointVertex ), &m_line[0].colour );
			gl().glVertexPointer( 3, GL_FLOAT, sizeof( PointVertex ), &m_line[0].vertex );
			gl().glDrawArrays( GL_LINES, 0, 2 );
		}
		void setColour( const Colour4b& colour ) {
			m_line[0].colour = colour;
			m_line[1].colour = colour;
		}
	};
	struct RenderableArrowHead : public OpenGLRenderable
	{
		Array<FlatShadedVertex> m_vertices;

		RenderableArrowHead( std::size_t size )
			: m_vertices( size ) {
		}
		void render( RenderStateFlags state ) const override {
			gl().glColorPointer( 4, GL_UNSIGNED_BYTE, sizeof( FlatShadedVertex ), &m_vertices.data()->colour );
			gl().glVertexPointer( 3, GL_FLOAT, sizeof( FlatShadedVertex ), &m_vertices.data()->vertex );
			gl().glNormalPointer( GL_FLOAT, sizeof( FlatShadedVertex ), &m_vertices.data()->normal );
			gl().glDrawArrays( GL_TRIANGLES, 0, GLsizei( m_vertices.size() ) );
		}
		void setColour( const Colour4b & colour ) {
			for( Array<FlatShadedVertex>::iterator i = m_vertices.begin(); i != m_vertices.end(); ++i ) {
				( *i ).colour = colour;
			}
		}
	};
	struct RenderablePoint : public OpenGLRenderable
	{
		PointVertex m_point;
		RenderablePoint():
			m_point( vertex3f_identity ) {
		}
		void render( RenderStateFlags state ) const override {
			gl().glColorPointer( 4, GL_UNSIGNED_BYTE, sizeof( PointVertex ), &m_point.colour );
			gl().glVertexPointer( 3, GL_FLOAT, sizeof( PointVertex ), &m_point.vertex );
			gl().glDrawArrays( GL_POINTS, 0, 1 );
		}
		void setColour( const Colour4b & colour ) {
			m_point.colour = colour;
		}
	};

	SkewAxis m_skew;
	TranslateFreeXY_Z m_translateFreeXY_Z;
	ScaleAxis m_scaleAxis;
	ScaleFree m_scaleFree;
	RotateAxis m_rotateAxis;
	AABB m_bounds_draw;
	const AABB& m_bounds;
	Matrix4& m_pivot2world;
	const bool& m_pivotIsCustom;
/*
	RenderableLine m_lineXy_;
	RenderableLine m_lineXy;
	RenderableLine m_lineXz_;
	RenderableLine m_lineXz;
	RenderableLine m_lineYz_;
	RenderableLine m_lineYz;
	RenderableLine m_lineYx_;
	RenderableLine m_lineYx;
	RenderableLine m_lineZx_;
	RenderableLine m_lineZx;
	RenderableLine m_lineZy_;
	RenderableLine m_lineZy;
*/
	RenderableLine m_lines[3][2][2];
	SelectableBool m_selectables[3][2][2];	//[X][YZ][-+]
	SelectableBool m_selectable_translateFree;
	DragPlanes m_selectables_scale; //+-X+-Y+-Z
	SelectableBool m_selectables_rotate[3][2][2];	//[X][-+Y][-+Z]
	Pivot2World m_pivot;
	Matrix4 m_worldSpace;
	RenderableArrowHead m_arrow;
	Matrix4 m_arrow_modelview;
	Matrix4 m_arrow_modelview2;
	RenderablePoint m_point;
public:
	static Shader* m_state_wire;
	static Shader* m_state_fill;
	static Shader* m_state_point;
	SkewManipulator( Skewable& skewable, Translatable& translatable, Scalable& scalable, Rotatable& rotatable, AllTransformable& transformable, const AABB& bounds, Matrix4& pivot2world, const bool& pivotIsCustom, const std::size_t segments = 2 ) :
		m_skew( skewable ),
		m_translateFreeXY_Z( translatable, transformable ),
		m_scaleAxis( scalable ),
		m_scaleFree( scalable ),
		m_rotateAxis( rotatable ),
		m_bounds( bounds ),
		m_pivot2world( pivot2world ),
		m_pivotIsCustom( pivotIsCustom ),
		m_selectables_scale( {} ),
		m_arrow( 3 * 2 * ( segments << 3 ) ) {
		for ( int i = 0; i < 3; ++i ){
			for ( int j = 0; j < 2; ++j ){
				const int x = i;
				const int y = ( i + j + 1 ) % 3;
				Vertex3f& xy_ = m_lines[i][j][0].m_line[0].vertex;
				Vertex3f& x_y_ = m_lines[i][j][0].m_line[1].vertex;
				Vertex3f& xy = m_lines[i][j][1].m_line[0].vertex;
				Vertex3f& x_y = m_lines[i][j][1].m_line[1].vertex;
				xy = x_y = xy_ = x_y_ = vertex3f_identity;
				xy[x] = xy_[x] = 1;
				x_y[x] = x_y_[x] = -1;
				xy[y] = x_y[y] = 1;
				xy_[y] = x_y_[y] = -1;
			}
		}
		draw_arrowhead( segments, 0, m_arrow.m_vertices.data(), TripleRemapXYZ<Vertex3f>(), TripleRemapXYZ<Normal3f>() );
		m_arrow.setColour( g_colour_selected );
		m_point.setColour( g_colour_selected );
	}

	void UpdateColours() {
		for ( int i = 0; i < 3; ++i )
			for ( int j = 0; j < 2; ++j )
				for ( int k = 0; k < 2; ++k )
					m_lines[i][j][k].setColour( colourSelected( g_colour_screen, m_selectables[i][j][k].isSelected() ) );
		for ( int i = 0; i < 3; ++i )
			for ( int j = 0; j < 2; ++j )
				if( m_selectables_scale.getSelectables()[i * 2 + j].isSelected() ){
					m_lines[( i + 1 ) % 3][1][j ^ 1].setColour( g_colour_selected );
					m_lines[( i + 2 ) % 3][0][j ^ 1].setColour( g_colour_selected );
				}
	}

	void updateModelview( const VolumeTest& volume, const Matrix4& pivot2world ){
		//m_pivot.update( pivot2world, volume.GetModelview(), volume.GetProjection(), volume.GetViewport() );
		//m_pivot.update( matrix4_translation_for_vec3( matrix4_get_translation_vec3( pivot2world ) ), volume.GetModelview(), volume.GetProjection(), volume.GetViewport() );
		m_pivot.update( matrix4_translation_for_vec3( m_bounds.origin ), volume.GetModelview(), volume.GetProjection(), volume.GetViewport() );
		//m_pivot.update( g_matrix4_identity, volume.GetModelview(), volume.GetProjection(), volume.GetViewport() ); //no shaking in cam due to low precision this way; smooth and sometimes very incorrect result
//		globalOutputStream() << m_pivot.m_worldSpace << '\n';
		Matrix4& m = m_pivot.m_worldSpace; /* go affine to increase precision */
		m[1] = m[2] = m[3] = m[4] = m[6] = m[7] = m[8] = m[9] = m[11] = 0;
		m[15] = 1;
		m_bounds_draw = aabb_for_oriented_aabb( m_bounds, matrix4_affine_inverse( m_pivot.m_worldSpace ) ); //screen scale
		for ( int i = 0; i < 3; ++i ){
			if( m_bounds_draw.extents[i] < 16 )
				m_bounds_draw.extents[i] = 18;
			else
				m_bounds_draw.extents[i] += 2.0f;
		}
		m_bounds_draw = aabb_for_oriented_aabb( m_bounds_draw, m_pivot.m_worldSpace ); //world scale
		m_bounds_draw.origin = m_bounds.origin;

		m_worldSpace = matrix4_multiplied_by_matrix4( matrix4_translation_for_vec3( m_bounds_draw.origin ), matrix4_scale_for_vec3( m_bounds_draw.extents ) );
		matrix4_premultiply_by_matrix4( m_worldSpace, matrix4_translation_for_vec3( -matrix4_get_translation_vec3( pivot2world ) ) );
		matrix4_premultiply_by_matrix4( m_worldSpace, pivot2world );

//		globalOutputStream() << m_worldSpace << '\n';
//		globalOutputStream() << pivot2world << '\n';
	}

	void render( Renderer& renderer, const VolumeTest& volume, const Matrix4& pivot2world ) override {
		updateModelview( volume, pivot2world );

		// temp hack
		UpdateColours();

		renderer.SetState( m_state_wire, Renderer::eWireframeOnly );
		renderer.SetState( m_state_wire, Renderer::eFullMaterials );

		for ( int i = 0; i < 3; ++i )
			for ( int j = 0; j < 2; ++j ){
#if 0
				const Vector3 dir = ( m_lines[i][j][0].m_line[0].vertex - m_lines[i][j][1].m_line[0].vertex ) / 2;
				const float dot = vector3_dot( dir, m_pivot.m_axis_screen );
				if( dot > 0.9999f )
					renderer.addRenderable( m_lines[i][j][0], m_worldSpace );
				else if( dot < -0.9999f )
					renderer.addRenderable( m_lines[i][j][1], m_worldSpace );
				else{
					renderer.addRenderable( m_lines[i][j][0], m_worldSpace );
					renderer.addRenderable( m_lines[i][j][1], m_worldSpace );
				}
#else
				if( m_selectables[i][j][0].isSelected() ){ /* add selected last to get highlighted one rendered on top in 2d */
					renderer.addRenderable( m_lines[i][j][1], m_worldSpace );
					renderer.addRenderable( m_lines[i][j][0], m_worldSpace );
				}
				else{
					renderer.addRenderable( m_lines[i][j][0], m_worldSpace );
					renderer.addRenderable( m_lines[i][j][1], m_worldSpace );
				}
#endif
			}

		for ( int i = 0; i < 3; ++i )
			for ( int j = 0; j < 2; ++j )
				for ( int k = 0; k < 2; ++k )
					if( m_selectables[i][j][k].isSelected() ){
						Vector3 origin = matrix4_transformed_point( m_worldSpace, m_lines[i][j][k].m_line[0].vertex );
						Vector3 origin2 = matrix4_transformed_point( m_worldSpace, m_lines[i][j][k].m_line[1].vertex );

						Pivot2World_worldSpace( m_arrow_modelview, matrix4_translation_for_vec3( origin ), volume.GetModelview(), volume.GetProjection(), volume.GetViewport() );
						Pivot2World_worldSpace( m_arrow_modelview2, matrix4_translation_for_vec3( origin2 ), volume.GetModelview(), volume.GetProjection(), volume.GetViewport() );

						const Matrix4 rot( i == 0? g_matrix4_identity: i == 1? matrix4_rotation_for_sincos_z( 1, 0 ): matrix4_rotation_for_sincos_y( -1, 0 ) );
						matrix4_multiply_by_matrix4( m_arrow_modelview, rot );
						matrix4_multiply_by_matrix4( m_arrow_modelview2, rot );
						const float x = 0.7f;
						matrix4_multiply_by_matrix4( m_arrow_modelview, matrix4_scale_for_vec3( Vector3( x, x, x ) ) );
						matrix4_multiply_by_matrix4( m_arrow_modelview2, matrix4_scale_for_vec3( Vector3( -x, x, x ) ) );

						renderer.SetState( m_state_fill, Renderer::eWireframeOnly );
						renderer.SetState( m_state_fill, Renderer::eFullMaterials );
						renderer.addRenderable( m_arrow, m_arrow_modelview );
						renderer.addRenderable( m_arrow, m_arrow_modelview2 );
						return;
					}

		for ( int i = 0; i < 3; ++i )
			for ( int j = 0; j < 2; ++j )
				for ( int k = 0; k < 2; ++k )
					if( m_selectables_rotate[i][j][k].isSelected() ){
						renderer.SetState( m_state_point, Renderer::eWireframeOnly );
						renderer.SetState( m_state_point, Renderer::eFullMaterials );
						renderer.addRenderable( m_point, m_worldSpace );
						renderer.addRenderable( m_point, m_worldSpace );
						return;
					}
	}
	void testSelect( const View& view, const Matrix4& pivot2world ) override {
		updateModelview( view, pivot2world );
		SelectionPool selector;
		const Matrix4 local2view( matrix4_multiplied_by_matrix4( view.GetViewMatrix(), m_worldSpace ) );

		if( g_modifiers == c_modifierAlt && view.fill() )
			goto testSelectBboxPlanes;
		if( g_modifiers != c_modifierNone )
			return selectionChange( nullptr );

		/* try corner points to rotate */
		for ( int i = 0; i < 3; ++i )
			for ( int j = 0; j < 2; ++j )
				for ( int k = 0; k < 2; ++k ){
					m_point.m_point.vertex[i] = 0;
					m_point.m_point.vertex[( i + 1 ) % 3] = j? 1 : -1;
					m_point.m_point.vertex[( i + 2 ) % 3] = k? 1 : -1;
					SelectionIntersection best;
					Point_BestPoint( local2view, m_point.m_point, best );
					selector.addSelectable( best, &m_selectables_rotate[i][j][k] );
				}
		if( !selector.failed() ) {
			( *selector.begin() ).second->setSelected( true );
			for ( int i = 0; i < 3; ++i )
				for ( int j = 0; j < 2; ++j )
					for ( int k = 0; k < 2; ++k )
						if( m_selectables_rotate[i][j][k].isSelected() ){
							m_point.m_point.vertex[i] = 0;
							m_point.m_point.vertex[( i + 1 ) % 3] = j? 1 : -1;
							m_point.m_point.vertex[( i + 2 ) % 3] = k? 1 : -1;
							if( !m_pivotIsCustom ){
								const Vector3 origin = m_bounds.origin + m_point.m_point.vertex * -1 * m_bounds.extents;
								m_pivot2world = matrix4_translation_for_vec3( origin );
							}
							/* set radius */
							if( fabs( vector3_dot( m_pivot.m_axis_screen, g_vector3_axes[i] ) ) < 0.2 ){
								Vector3 origin = matrix4_get_translation_vec3( m_pivot2world );
								Vector3 point = m_bounds_draw.origin + m_point.m_point.vertex * m_bounds_draw.extents;
								const Matrix4 inv = matrix4_affine_inverse( m_pivot.m_worldSpace );
								matrix4_transform_point( inv, origin );
								matrix4_transform_point( inv, point );
								point -= origin;
								point = vector3_added( point, vector3_scaled( m_pivot.m_axis_screen, -vector3_dot( point, m_pivot.m_axis_screen ) ) ); //constrain_to_axis
								m_rotateAxis.SetRadius( vector3_length( point ) - g_SELECT_EPSILON / 2.0 - 1.0 ); /* use smaller radius to constrain to one rotation direction in 2D */
								//globalOutputStream() << "radius " << ( vector3_length( point ) - g_SELECT_EPSILON / 2.0 - 1.0 ) << '\n';
							}
							else{
								m_rotateAxis.SetRadius( g_radius );
								//globalOutputStream() << "g_radius\n";
							}
						}
		}
		else{
			/* try lines to skew */
			for ( int i = 0; i < 3; ++i )
				for ( int j = 0; j < 2; ++j )
					for ( int k = 0; k < 2; ++k ){
						SelectionIntersection best;
						Line_BestPoint( local2view, m_lines[i][j][k].m_line, best );
						selector.addSelectable( best, &m_selectables[i][j][k] );
					}

			if( !selector.failed() ) {
				( *selector.begin() ).second->setSelected( true );
				m_skew.set0( vector4_projected( matrix4_transformed_vector4( matrix4_full_inverse( view.GetViewMatrix() ), Vector4( 0, 0, selector.begin()->first.depth(), 1 ) ) ) );
				if( !m_pivotIsCustom )
					for ( int i = 0; i < 3; ++i )
						for ( int j = 0; j < 2; ++j )
							for ( int k = 0; k < 2; ++k )
								if( m_selectables[i][j][k].isSelected() ){
									const int axis_by = ( i + j + 1 ) % 3;
									Vector3 origin = m_bounds.origin;
									origin[axis_by] += k? -m_bounds.extents[axis_by] : m_bounds.extents[axis_by];
									m_pivot2world = matrix4_translation_for_vec3( origin );
								}
			}
			else{ /* try bbox to translate */
				SelectionIntersection best;
				AABB_BestPoint( local2view, eClipCullCW, AABB( Vector3( 0, 0, 0 ), Vector3( 1, 1, 1 ) ), best );
				selector.addSelectable( best, &m_selectable_translateFree );
				if( !selector.failed() )
					m_translateFreeXY_Z.set0( vector4_projected( matrix4_transformed_vector4( matrix4_full_inverse( view.GetViewMatrix() ), Vector4( 0, 0, selector.begin()->first.depth(), 1 ) ) ) );
			}
		}
testSelectBboxPlanes:
		/* try bbox planes to scale */
		if( selector.failed() ){
			SelectionVolume test( view );
			test.BeginMesh( g_matrix4_identity, true );

			if( g_modifiers == c_modifierAlt ){
				PlaneSelectable::BestPlaneData planeData;
				m_selectables_scale.bestPlaneDirect( m_bounds_draw, test, planeData );
				if( !planeData.valid() ){
					m_selectables_scale.bestPlaneIndirect( m_bounds_draw, test, planeData );
				}
				if( planeData.valid() ){
					m_selectables_scale.selectByPlane( m_bounds_draw, planeData.m_plane );
				}
			}
			else{
				m_selectables_scale.selectPlanes( m_bounds_draw, selector, test, {} );
				for( auto& [ intersection, selectable ] : selector )
					selectable->setSelected( true );
			}

			std::uintptr_t newsel = 0;
			Vector3 origin = m_bounds.origin;
			for ( int i = 0; i < 3; ++i )
				for ( int j = 0; j < 2; ++j )
					if( m_selectables_scale.getSelectables()[i * 2 + j].isSelected() ){
						origin[i] += j? m_bounds.extents[i] : -m_bounds.extents[i];
						newsel += reinterpret_cast<std::uintptr_t>( &m_selectables_scale.getSelectables()[i * 2 + j] ); // hack: store up to 2 pointers in one
					}
			if( !m_pivotIsCustom )
				m_pivot2world = matrix4_translation_for_vec3( origin );
			return selectionChange( reinterpret_cast<const ObservedSelectable *>( newsel ) );
		}

		selectionChange( selector );
	}

	Manipulatable* GetManipulatable() override {
		for ( int i = 0; i < 3; ++i )
			for ( int j = 0; j < 2; ++j )
				for ( int k = 0; k < 2; ++k )
					if( m_selectables[i][j][k].isSelected() ){
						m_skew.SetAxes( i, ( i + j + 1 ) % 3, k? 1 : -1 );
						return &m_skew;
					}
					else if( m_selectables_rotate[i][j][k].isSelected() ){
						m_rotateAxis.SetAxis( g_vector3_axes[i] );
						return &m_rotateAxis;
					}
		{
			Vector3 axes[2] = { g_vector3_identity, g_vector3_identity };
			Vector3* axis = axes;
			for ( int i = 0; i < 3; ++i )
				for ( int j = 0; j < 2; ++j )
					if( m_selectables_scale.getSelectables()[i * 2 + j].isSelected() )
						( *axis++ )[i] = j? -1 : 1;
			if( axis - axes == 2 ){
				m_scaleFree.SetAxes( axes[0], axes[1] );
				return &m_scaleFree;
			}
			else if( axis - axes == 1 ){
				m_scaleAxis.SetAxis( axes[0] );
				return &m_scaleAxis;
			}
		}
		return &m_translateFreeXY_Z;
	}

	void setSelected( bool select ) override {
		m_selectable_translateFree.setSelected( select );
		for ( int i = 0; i < 3; ++i )
			for ( int j = 0; j < 2; ++j )
				for ( int k = 0; k < 2; ++k ){
					m_selectables[i][j][k].setSelected( select );
					m_selectables_rotate[i][j][k].setSelected( select );
				}
		m_selectables_scale.setSelected( select );
	}
	bool isSelected() const override {
		bool selected = false;
		for ( int i = 0; i < 3; ++i )
			for ( int j = 0; j < 2; ++j )
				for ( int k = 0; k < 2; ++k ){
					selected |= m_selectables[i][j][k].isSelected();
					selected |= m_selectables_rotate[i][j][k].isSelected();
				}
		selected |= m_selectables_scale.isSelected();
		return selected | m_selectable_translateFree.isSelected();
	}
};

Shader* SkewManipulator::m_state_wire;
Shader* SkewManipulator::m_state_fill;
Shader* SkewManipulator::m_state_point;



inline PlaneSelectable* Instance_getPlaneSelectable( scene::Instance& instance ){
	return InstanceTypeCast<PlaneSelectable>::cast( instance );
}

class PlaneSelectableSelectPlanes : public scene::Graph::Walker
{
	Selector& m_selector;
	SelectionTest& m_test;
	PlaneCallback m_selectedPlaneCallback;
public:
	PlaneSelectableSelectPlanes( Selector& selector, SelectionTest& test, const PlaneCallback& selectedPlaneCallback )
		: m_selector( selector ), m_test( test ), m_selectedPlaneCallback( selectedPlaneCallback ){
	}
	bool pre( const scene::Path& path, scene::Instance& instance ) const override {
		if ( path.top().get().visible() && Instance_isSelected( instance ) ) {
			PlaneSelectable* planeSelectable = Instance_getPlaneSelectable( instance );
			if ( planeSelectable != 0 ) {
				planeSelectable->selectPlanes( m_selector, m_test, m_selectedPlaneCallback );
			}
		}
		return true;
	}
};

class PlaneSelectableSelectReversedPlanes : public scene::Graph::Walker
{
	Selector& m_selector;
	const SelectedPlanes& m_selectedPlanes;
public:
	PlaneSelectableSelectReversedPlanes( Selector& selector, const SelectedPlanes& selectedPlanes )
		: m_selector( selector ), m_selectedPlanes( selectedPlanes ){
	}
	bool pre( const scene::Path& path, scene::Instance& instance ) const override {
		if ( path.top().get().visible() && Instance_isSelected( instance ) ) {
			PlaneSelectable* planeSelectable = Instance_getPlaneSelectable( instance );
			if ( planeSelectable != 0 ) {
				planeSelectable->selectReversedPlanes( m_selector, m_selectedPlanes );
			}
		}
		return true;
	}
};

void Scene_forEachPlaneSelectable_selectPlanes( scene::Graph& graph, Selector& selector, SelectionTest& test, const PlaneCallback& selectedPlaneCallback ){
	graph.traverse( PlaneSelectableSelectPlanes( selector, test, selectedPlaneCallback ) );
}

void Scene_forEachPlaneSelectable_selectReversedPlanes( scene::Graph& graph, Selector& selector, const SelectedPlanes& selectedPlanes ){
	graph.traverse( PlaneSelectableSelectReversedPlanes( selector, selectedPlanes ) );
}


class PlaneLess
{
public:
	bool operator()( const Plane3& plane, const Plane3& other ) const {
		if ( plane.a < other.a ) {
			return true;
		}
		if ( other.a < plane.a ) {
			return false;
		}

		if ( plane.b < other.b ) {
			return true;
		}
		if ( other.b < plane.b ) {
			return false;
		}

		if ( plane.c < other.c ) {
			return true;
		}
		if ( other.c < plane.c ) {
			return false;
		}

		if ( plane.d < other.d ) {
			return true;
		}
		if ( other.d < plane.d ) {
			return false;
		}

		return false;
	}
};

typedef std::set<Plane3, PlaneLess> PlaneSet;


class SelectedPlaneSet : public SelectedPlanes
{
	PlaneSet m_selectedPlanes;
public:
	bool empty() const {
		return m_selectedPlanes.empty();
	}

	void insert( const Plane3& plane ){
		m_selectedPlanes.insert( plane );
	}
	bool contains( const Plane3& plane ) const override {
		return m_selectedPlanes.contains( plane );
	}
	typedef MemberCaller<SelectedPlaneSet, void(const Plane3&), &SelectedPlaneSet::insert> InsertCaller;
};


bool Scene_forEachPlaneSelectable_selectPlanes( scene::Graph& graph, Selector& selector, SelectionTest& test ){
	SelectedPlaneSet selectedPlanes;

	Scene_forEachPlaneSelectable_selectPlanes( graph, selector, test, SelectedPlaneSet::InsertCaller( selectedPlanes ) );
	Scene_forEachPlaneSelectable_selectReversedPlanes( graph, selector, selectedPlanes );

	return !selectedPlanes.empty();
}




template<typename Functor>
class PlaneselectableVisibleSelectedVisitor : public SelectionSystem::Visitor
{
	const Functor& m_functor;
public:
	PlaneselectableVisibleSelectedVisitor( const Functor& functor ) : m_functor( functor ){
	}
	void visit( scene::Instance& instance ) const override {
		PlaneSelectable* planeSelectable = Instance_getPlaneSelectable( instance );
		if ( planeSelectable != 0
		     && instance.path().top().get().visible() ) {
			m_functor( *planeSelectable );
		}
	}
};

template<typename Functor>
inline const Functor& Scene_forEachVisibleSelectedPlaneselectable( const Functor& functor ){
	GlobalSelectionSystem().foreachSelected( PlaneselectableVisibleSelectedVisitor<Functor>( functor ) );
	return functor;
}

PlaneSelectable::BestPlaneData Scene_forEachPlaneSelectable_bestPlane( SelectionTest& test ){
	PlaneSelectable::BestPlaneData planeData;
	auto bestPlaneDirect = [&test, &planeData]( PlaneSelectable& planeSelectable ){
		planeSelectable.bestPlaneDirect( test, planeData );
	};
	Scene_forEachVisibleSelectedPlaneselectable( bestPlaneDirect );
	if( !planeData.valid() ){
		auto bestPlaneIndirect = [&test, &planeData]( PlaneSelectable& planeSelectable ){
			planeSelectable.bestPlaneIndirect( test, planeData );
		};
		Scene_forEachVisibleSelectedPlaneselectable( bestPlaneIndirect );
	}
	return planeData;
}

bool Scene_forEachPlaneSelectable_selectPlanes2( SelectionTest& test, TranslateAxis2& translateAxis ){
	const auto planeData = Scene_forEachPlaneSelectable_bestPlane( test );

	if( planeData.valid() ){
		const Plane3 plane = planeData.m_plane;
		if( planeData.direct() ){ // direct
			translateAxis.set0( point_on_plane( plane, test.getVolume().GetViewMatrix(), DeviceVector( 0, 0 ) ), plane );
		}
		else{ // indirect
			test.BeginMesh( g_matrix4_identity );
			/* may introduce some screen space offset in manipulatable to handle far-from-edge clicks perfectly; thought clicking not so far isn't too nasty, right? */
			translateAxis.set0( vector4_projected( matrix4_transformed_vector4( test.getScreen2world(), Vector4( planeData.m_closestPoint, 1 ) ) ), plane );
		}

		auto selectByPlane = [plane]( PlaneSelectable& planeSelectable ){
			planeSelectable.selectByPlane( plane );
		};
		Scene_forEachVisibleSelectedPlaneselectable( selectByPlane );
	}

	return planeData.valid();
}


PlaneSelectable::BestPlaneData Scene_forEachSelectedBrush_bestPlane( SelectionTest& test ){
	PlaneSelectable::BestPlaneData planeData;
	auto bestPlaneDirect = [&test, &planeData]( BrushInstance& brushInstance ){
		brushInstance.bestPlaneDirect( test, planeData );
	};
	Scene_forEachVisibleSelectedBrush( bestPlaneDirect );
	if( !planeData.valid() ){
		auto bestPlaneIndirect = [&test, &planeData]( BrushInstance& brushInstance ){
			brushInstance.bestPlaneIndirect( test, planeData );
		};
		Scene_forEachVisibleSelectedBrush( bestPlaneIndirect );
	}
	return planeData;
}

PlaneSelectable::BestPlaneData Scene_forEachBrush_bestPlane( SelectionTest& test ){
	if( g_SelectedFaceInstances.empty() ){
		return Scene_forEachSelectedBrush_bestPlane( test );
	}
	else{
		PlaneSelectable::BestPlaneData planeData;
		auto bestPlaneDirect = [&test, &planeData]( BrushInstance& brushInstance ){
			if( brushInstance.isSelected() || brushInstance.isSelectedComponents() )
				brushInstance.bestPlaneDirect( test, planeData );
		};

		Scene_forEachVisibleBrush( GlobalSceneGraph(), bestPlaneDirect );
		if( !planeData.valid() ){
			auto bestPlaneIndirect = [&test, &planeData]( BrushInstance& brushInstance ){
				if( brushInstance.isSelected() || brushInstance.isSelectedComponents() )
					brushInstance.bestPlaneIndirect( test, planeData );
			};
			Scene_forEachVisibleBrush( GlobalSceneGraph(), bestPlaneIndirect );
		}
		return planeData;
	}
}

bool Scene_forEachBrush_setupExtrude( SelectionTest& test, DragExtrudeFaces& extrudeFaces ){
	const auto planeData = Scene_forEachBrush_bestPlane( test );

	if( planeData.valid() ){
		const Plane3 plane = planeData.m_plane;
		if( planeData.direct() ){ // direct
			extrudeFaces.set0( point_on_plane( plane, test.getVolume().GetViewMatrix(), DeviceVector( 0, 0 ) ), plane );
		}
		else{ // indirect
			test.BeginMesh( g_matrix4_identity );
			/* may introduce some screen space offset in manipulatable to handle far-from-edge clicks perfectly; thought clicking not so far isn't too nasty, right? */
			extrudeFaces.set0( vector4_projected( matrix4_transformed_vector4( test.getScreen2world(), Vector4( planeData.m_closestPoint, 1 ) ) ), plane );
		}
		extrudeFaces.m_extrudeSources.clear();
		auto gatherExtrude = [plane, &extrudeFaces]( BrushInstance& brushInstance ){
			if( brushInstance.isSelected() || brushInstance.isSelectedComponents() ){
				bool m_pushed = false;
				auto gatherFaceInstances = [plane, &extrudeFaces, &brushInstance, &m_pushed]( FaceInstance& face ){
					if( face.isSelected() || plane3_equal( plane, face.getFace().plane3() ) ){
						if( !m_pushed ){
							extrudeFaces.m_extrudeSources.emplace_back();
							extrudeFaces.m_extrudeSources.back().m_brushInstance = &brushInstance;
							m_pushed = true;
						}
						extrudeFaces.m_extrudeSources.back().m_faces.emplace_back();
						extrudeFaces.m_extrudeSources.back().m_faces.back().m_face = &face.getFace();
						planepts_assign( extrudeFaces.m_extrudeSources.back().m_faces.back().m_planepoints, face.getFace().getPlane().getPlanePoints() );
					}
				};
				Brush_ForEachFaceInstance( brushInstance, gatherFaceInstances );

				brushInstance.setSelectedComponents( false, SelectionSystem::eFace );
				brushInstance.setSelected( false );
			}
		};
		Scene_forEachVisibleBrush( GlobalSceneGraph(), gatherExtrude );
	}

	return planeData.valid();
}



void Scene_Translate_Component_Selected( scene::Graph& graph, const Vector3& translation );
void Scene_Translate_Selected( scene::Graph& graph, const Vector3& translation );
void Scene_TestSelect_Primitive( Selector& selector, SelectionTest& test, const VolumeTest& volume );
void Scene_TestSelect_Component( Selector& selector, SelectionTest& test, const VolumeTest& volume, SelectionSystem::EComponentMode componentMode );
void Scene_TestSelect_Component_Selected( Selector& selector, SelectionTest& test, const VolumeTest& volume, SelectionSystem::EComponentMode componentMode );
void Scene_SelectAll_Component( bool select, SelectionSystem::EComponentMode componentMode );

class ResizeTranslatable : public Translatable
{
	void translate( const Vector3& translation ) override {
		Scene_Translate_Component_Selected( GlobalSceneGraph(), translation );
	}
};


class SelectionCounter
{
public:
	using func = void(const Selectable &);

	SelectionCounter( const SelectionChangeCallback& onchanged )
		: m_count( 0 ), m_onchanged( onchanged ){
	}
	void operator()( const Selectable& selectable ){
		if ( selectable.isSelected() ) {
			++m_count;
		}
		else
		{
			ASSERT_MESSAGE( m_count != 0, "selection counter underflow" );
			--m_count;
		}

		m_onchanged( selectable );
	}
	bool empty() const {
		return m_count == 0;
	}
	std::size_t size() const {
		return m_count;
	}
private:
	std::size_t m_count;
	SelectionChangeCallback m_onchanged;
};

class SelectedStuffCounter
{
public:
	std::size_t m_brushcount;
	std::size_t m_patchcount;
	std::size_t m_entitycount;
	SelectedStuffCounter() : m_brushcount( 0 ), m_patchcount( 0 ), m_entitycount( 0 ){
	}
	void increment( scene::Node& node ) {
		if( Node_isBrush( node ) )
			++m_brushcount;
		else if( Node_isPatch( node ) )
			++m_patchcount;
		else if( Node_isEntity( node ) )
			++m_entitycount;
	}
	void decrement( scene::Node& node ) {
		if( Node_isBrush( node ) )
			--m_brushcount;
		else if( Node_isPatch( node ) )
			--m_patchcount;
		else if( Node_isEntity( node ) )
			--m_entitycount;
	}
	void get( std::size_t& brushes, std::size_t& patches, std::size_t& entities ) const {
		brushes = m_brushcount;
		patches = m_patchcount;
		entities = m_entitycount;
	}
};

inline void ConstructSelectionTest( View& view, const rect_t selection_box ){
	view.EnableScissor( selection_box.min[0], selection_box.max[0], selection_box.min[1], selection_box.max[1] );
}

inline const rect_t SelectionBoxForPoint( const DeviceVector& device_point, const DeviceVector& device_epsilon ){
	rect_t selection_box;
	selection_box.min[0] = device_point[0] - device_epsilon[0];
	selection_box.min[1] = device_point[1] - device_epsilon[1];
	selection_box.max[0] = device_point[0] + device_epsilon[0];
	selection_box.max[1] = device_point[1] + device_epsilon[1];
	return selection_box;
}

inline const rect_t SelectionBoxForArea( const DeviceVector& device_point, const DeviceVector& device_delta ){
	rect_t selection_box;
	selection_box.min[0] = device_point[0] + std::min( device_delta[0], 0.f );
	selection_box.min[1] = device_point[1] + std::min( device_delta[1], 0.f );
	selection_box.max[0] = device_point[0] + std::max( device_delta[0], 0.f );
	selection_box.max[1] = device_point[1] + std::max( device_delta[1], 0.f );
	selection_box.modifier = device_delta[0] * device_delta[1] < 0?
	                         rect_t::eToggle
	                         : device_delta[0] < 0 ?
	                         rect_t::eDeselect
	                         : rect_t::eSelect;
	return selection_box;
}
#if 0
Quaternion construct_local_rotation( const Quaternion& world, const Quaternion& localToWorld ){
	return quaternion_normalised( quaternion_multiplied_by_quaternion(
	                                  quaternion_normalised( quaternion_multiplied_by_quaternion(
	                                          quaternion_inverse( localToWorld ),
	                                          world
	                                          ) ),
	                                  localToWorld
	                              ) );
}
#endif
inline void matrix4_assign_rotation( Matrix4& matrix, const Matrix4& other ){
	matrix[0] = other[0];
	matrix[1] = other[1];
	matrix[2] = other[2];
	matrix[4] = other[4];
	matrix[5] = other[5];
	matrix[6] = other[6];
	matrix[8] = other[8];
	matrix[9] = other[9];
	matrix[10] = other[10];
}
#define SELECTIONSYSTEM_AXIAL_PIVOTS
void matrix4_assign_rotation_for_pivot( Matrix4& matrix, scene::Instance& instance ){
#ifndef SELECTIONSYSTEM_AXIAL_PIVOTS
	Editable* editable = Node_getEditable( instance.path().top() );
	if ( editable != 0 ) {
		matrix4_assign_rotation( matrix, matrix4_multiplied_by_matrix4( instance.localToWorld(), editable->getLocalPivot() ) );
	}
	else
	{
		matrix4_assign_rotation( matrix, instance.localToWorld() );
	}
#endif
}

class TranslateSelected : public SelectionSystem::Visitor
{
	const Vector3& m_translate;
public:
	TranslateSelected( const Vector3& translate )
		: m_translate( translate ){
	}
	void visit( scene::Instance& instance ) const override {
		Transformable* transform = Instance_getTransformable( instance );
		if ( transform != 0 ) {
			transform->setType( TRANSFORM_PRIMITIVE );
			transform->setRotation( c_rotation_identity );
			transform->setTranslation( m_translate );
		}
	}
};

void Scene_Translate_Selected( scene::Graph& graph, const Vector3& translation ){
	if ( GlobalSelectionSystem().countSelected() != 0 ) {
		GlobalSelectionSystem().foreachSelected( TranslateSelected( translation ) );
	}
}

Vector3 get_local_pivot( const Vector3& world_pivot, const Matrix4& localToWorld ){
	return matrix4_transformed_point(
	           matrix4_full_inverse( localToWorld ),
	           world_pivot
	       );
}

void translation_for_pivoted_matrix_transform( Vector3& parent_translation, const Matrix4& local_transform, const Vector3& world_pivot, const Matrix4& localToWorld, const Matrix4& localToParent ){
	// we need a translation inside the parent system to move the origin of this object to the right place

	// mathematically, it must fulfill:
	//
	//   local_translation local_transform local_pivot = local_pivot
	//   local_translation = local_pivot - local_transform local_pivot
	//
	//   or maybe?
	//   local_transform local_translation local_pivot = local_pivot
	//                   local_translation local_pivot = local_transform^-1 local_pivot
	//                 local_translation + local_pivot = local_transform^-1 local_pivot
	//                   local_translation             = local_transform^-1 local_pivot - local_pivot

	Vector3 local_pivot( get_local_pivot( world_pivot, localToWorld ) );

	Vector3 local_translation(
	    vector3_subtracted(
	        local_pivot,
	        matrix4_transformed_point(
	            local_transform,
	            local_pivot
	        )
	        /*
	            matrix4_transformed_point(
	                matrix4_full_inverse( local_transform ),
	                local_pivot
	            ),
	            local_pivot
	         */
	    )
	);

	parent_translation = translation_local2object( local_translation, localToParent );

	/*
	   // verify it!
	   globalOutputStream() << "World pivot is at " << world_pivot << '\n';
	   globalOutputStream() << "Local pivot is at " << local_pivot << '\n';
	   globalOutputStream() << "Transformation " << local_transform << " moves it to: " << matrix4_transformed_point( local_transform, local_pivot ) << '\n';
	   globalOutputStream() << "Must move by " << local_translation << " in the local system" << '\n';
	   globalOutputStream() << "Must move by " << parent_translation << " in the parent system" << '\n';
	 */
}

void translation_for_pivoted_rotation( Vector3& parent_translation, const Quaternion& local_rotation, const Vector3& world_pivot, const Matrix4& localToWorld, const Matrix4& localToParent ){
	translation_for_pivoted_matrix_transform( parent_translation, matrix4_rotation_for_quaternion_quantised( local_rotation ), world_pivot, localToWorld, localToParent );
}

void translation_for_pivoted_scale( Vector3& parent_translation, const Vector3& world_scale, const Vector3& world_pivot, const Matrix4& localToWorld, const Matrix4& localToParent ){
	Matrix4 local_transform(
	    matrix4_multiplied_by_matrix4(
	        matrix4_full_inverse( localToWorld ),
	        matrix4_multiplied_by_matrix4(
	            matrix4_scale_for_vec3( world_scale ),
	            localToWorld
	        )
	    )
	);
	local_transform.tx() = local_transform.ty() = local_transform.tz() = 0; // cancel translation parts
	translation_for_pivoted_matrix_transform( parent_translation, local_transform, world_pivot, localToWorld, localToParent );
}

void translation_for_pivoted_skew( Vector3& parent_translation, const Skew& local_skew, const Vector3& world_pivot, const Matrix4& localToWorld, const Matrix4& localToParent ){
	Matrix4 local_transform( g_matrix4_identity );
	local_transform[local_skew.index] = local_skew.amount;
	translation_for_pivoted_matrix_transform( parent_translation, local_transform, world_pivot, localToWorld, localToParent );
}

class rotate_selected : public SelectionSystem::Visitor
{
	const Quaternion& m_rotate;
	const Vector3& m_world_pivot;
public:
	rotate_selected( const Quaternion& rotation, const Vector3& world_pivot )
		: m_rotate( rotation ), m_world_pivot( world_pivot ){
	}
	void visit( scene::Instance& instance ) const override {
		TransformNode* transformNode = Node_getTransformNode( instance.path().top() );
		if ( transformNode != 0 ) {
			Transformable* transform = Instance_getTransformable( instance );
			if ( transform != 0 ) {
				transform->setType( TRANSFORM_PRIMITIVE );
				transform->setScale( c_scale_identity );
				transform->setTranslation( c_translation_identity );

				transform->setType( TRANSFORM_PRIMITIVE );
				transform->setRotation( m_rotate );

				{
					Editable* editable = Node_getEditable( instance.path().top() );
					const Matrix4& localPivot = editable != 0 ? editable->getLocalPivot() : g_matrix4_identity;

					Vector3 parent_translation;
					translation_for_pivoted_rotation(
					    parent_translation,
					    m_rotate,
					    m_world_pivot,
#ifdef SELECTIONSYSTEM_AXIAL_PIVOTS
					    matrix4_multiplied_by_matrix4( matrix4_translation_for_vec3( matrix4_get_translation_vec3( instance.localToWorld() ) ), localPivot ),
					    matrix4_multiplied_by_matrix4( matrix4_translation_for_vec3( matrix4_get_translation_vec3( transformNode->localToParent() ) ), localPivot )
#else
					    matrix4_multiplied_by_matrix4( instance.localToWorld(), localPivot ),
					    matrix4_multiplied_by_matrix4( transformNode->localToParent(), localPivot )
#endif
					);

					transform->setTranslation( parent_translation );
				}
			}
		}
	}
};

void Scene_Rotate_Selected( scene::Graph& graph, const Quaternion& rotation, const Vector3& world_pivot ){
	if ( GlobalSelectionSystem().countSelected() != 0 ) {
		GlobalSelectionSystem().foreachSelected( rotate_selected( rotation, world_pivot ) );
	}
}

class scale_selected : public SelectionSystem::Visitor
{
	const Vector3& m_scale;
	const Vector3& m_world_pivot;
public:
	scale_selected( const Vector3& scaling, const Vector3& world_pivot )
		: m_scale( scaling ), m_world_pivot( world_pivot ){
	}
	void visit( scene::Instance& instance ) const override {
		TransformNode* transformNode = Node_getTransformNode( instance.path().top() );
		if ( transformNode != 0 ) {
			Transformable* transform = Instance_getTransformable( instance );
			if ( transform != 0 ) {
				transform->setType( TRANSFORM_PRIMITIVE );
				transform->setScale( c_scale_identity );
				transform->setTranslation( c_translation_identity );

				transform->setType( TRANSFORM_PRIMITIVE );
				transform->setScale( m_scale );
				{
					Editable* editable = Node_getEditable( instance.path().top() );
					const Matrix4& localPivot = editable != 0 ? editable->getLocalPivot() : g_matrix4_identity;

					Vector3 parent_translation;
					translation_for_pivoted_scale(
					    parent_translation,
					    m_scale,
					    m_world_pivot,
					    matrix4_multiplied_by_matrix4( instance.localToWorld(), localPivot ),
					    matrix4_multiplied_by_matrix4( transformNode->localToParent(), localPivot )
					);

					transform->setTranslation( parent_translation );
				}
			}
		}
	}
};

void Scene_Scale_Selected( scene::Graph& graph, const Vector3& scaling, const Vector3& world_pivot ){
	if ( GlobalSelectionSystem().countSelected() != 0 ) {
		GlobalSelectionSystem().foreachSelected( scale_selected( scaling, world_pivot ) );
	}
}

class skew_selected : public SelectionSystem::Visitor
{
	const Skew& m_skew;
	const Vector3& m_world_pivot;
public:
	skew_selected( const Skew& skew, const Vector3& world_pivot )
		: m_skew( skew ), m_world_pivot( world_pivot ){
	}
	void visit( scene::Instance& instance ) const override {
		TransformNode* transformNode = Node_getTransformNode( instance.path().top() );
		if ( transformNode != 0 ) {
			Transformable* transform = Instance_getTransformable( instance );
			if ( transform != 0 ) {
				transform->setType( TRANSFORM_PRIMITIVE );
				transform->setScale( c_scale_identity );
				transform->setTranslation( c_translation_identity );

				transform->setType( TRANSFORM_PRIMITIVE );
				transform->setSkew( m_skew );
				{
					Editable* editable = Node_getEditable( instance.path().top() );
					const Matrix4& localPivot = editable != 0 ? editable->getLocalPivot() : g_matrix4_identity;

					Vector3 parent_translation;
					translation_for_pivoted_skew(
					    parent_translation,
					    m_skew,
					    m_world_pivot,
					    matrix4_multiplied_by_matrix4( matrix4_translation_for_vec3( matrix4_get_translation_vec3( instance.localToWorld() ) ), localPivot ),
					    matrix4_multiplied_by_matrix4( matrix4_translation_for_vec3( matrix4_get_translation_vec3( transformNode->localToParent() ) ), localPivot )
					);

					transform->setTranslation( parent_translation );
				}
			}
		}
	}
};

void Scene_Skew_Selected( scene::Graph& graph, const Skew& skew, const Vector3& world_pivot ){
	if ( GlobalSelectionSystem().countSelected() != 0 ) {
		GlobalSelectionSystem().foreachSelected( skew_selected( skew, world_pivot ) );
	}
}

class transform_selected : public SelectionSystem::Visitor
{
	const Transforms& m_transforms;
	const Vector3& m_world_pivot;
public:
	transform_selected( const Transforms& transforms, const Vector3& world_pivot )
		: m_transforms( transforms ), m_world_pivot( world_pivot ){
	}
	void visit( scene::Instance& instance ) const override {
		TransformNode* transformNode = Node_getTransformNode( instance.path().top() );
		if ( transformNode != 0 ) {
			Transformable* transform = Instance_getTransformable( instance );
			if ( transform != 0 ) {
				transform->setType( TRANSFORM_PRIMITIVE );
				transform->setRotation( m_transforms.getRotation() );
				transform->setScale( m_transforms.getScale() );
				transform->setSkew( m_transforms.getSkew() );
				transform->setTranslation( c_translation_identity );
				{
					Editable* editable = Node_getEditable( instance.path().top() );
					const Matrix4& localPivot = editable != 0 ? editable->getLocalPivot() : g_matrix4_identity;

					const Matrix4 local_transform = matrix4_transform_for_components( c_translation_identity, m_transforms.getRotation(), m_transforms.getScale(), m_transforms.getSkew() );
					Vector3 parent_translation;
					translation_for_pivoted_matrix_transform(
					    parent_translation,
					    local_transform,
					    m_world_pivot,
					    matrix4_multiplied_by_matrix4( matrix4_translation_for_vec3( matrix4_get_translation_vec3( instance.localToWorld() ) ), localPivot ),
					    matrix4_multiplied_by_matrix4( matrix4_translation_for_vec3( matrix4_get_translation_vec3( transformNode->localToParent() ) ), localPivot )
					);

					transform->setTranslation( parent_translation + m_transforms.getTranslation() );
				}
			}
		}
	}
};


class translate_component_selected : public SelectionSystem::Visitor
{
	const Vector3& m_translate;
public:
	translate_component_selected( const Vector3& translate )
		: m_translate( translate ){
	}
	void visit( scene::Instance& instance ) const override {
		Transformable* transform = Instance_getTransformable( instance );
		if ( transform != 0 ) {
			transform->setType( TRANSFORM_COMPONENT );
			transform->setRotation( c_rotation_identity );
			transform->setTranslation( m_translate );
		}
	}
};

void Scene_Translate_Component_Selected( scene::Graph& graph, const Vector3& translation ){
	if ( GlobalSelectionSystem().countSelected() != 0 ) {
		GlobalSelectionSystem().foreachSelectedComponent( translate_component_selected( translation ) );
	}
}

class rotate_component_selected : public SelectionSystem::Visitor
{
	const Quaternion& m_rotate;
	const Vector3& m_world_pivot;
public:
	rotate_component_selected( const Quaternion& rotation, const Vector3& world_pivot )
		: m_rotate( rotation ), m_world_pivot( world_pivot ){
	}
	void visit( scene::Instance& instance ) const override {
		Transformable* transform = Instance_getTransformable( instance );
		if ( transform != 0 ) {
			Vector3 parent_translation;
			translation_for_pivoted_rotation( parent_translation, m_rotate, m_world_pivot, instance.localToWorld(), Node_getTransformNode( instance.path().top() )->localToParent() );

			transform->setType( TRANSFORM_COMPONENT );
			transform->setRotation( m_rotate );
			transform->setTranslation( parent_translation );
		}
	}
};

void Scene_Rotate_Component_Selected( scene::Graph& graph, const Quaternion& rotation, const Vector3& world_pivot ){
	if ( GlobalSelectionSystem().countSelectedComponents() != 0 ) {
		GlobalSelectionSystem().foreachSelectedComponent( rotate_component_selected( rotation, world_pivot ) );
	}
}

class scale_component_selected : public SelectionSystem::Visitor
{
	const Vector3& m_scale;
	const Vector3& m_world_pivot;
public:
	scale_component_selected( const Vector3& scaling, const Vector3& world_pivot )
		: m_scale( scaling ), m_world_pivot( world_pivot ){
	}
	void visit( scene::Instance& instance ) const override {
		Transformable* transform = Instance_getTransformable( instance );
		if ( transform != 0 ) {
			Vector3 parent_translation;
			translation_for_pivoted_scale( parent_translation, m_scale, m_world_pivot, instance.localToWorld(), Node_getTransformNode( instance.path().top() )->localToParent() );

			transform->setType( TRANSFORM_COMPONENT );
			transform->setScale( m_scale );
			transform->setTranslation( parent_translation );
		}
	}
};

void Scene_Scale_Component_Selected( scene::Graph& graph, const Vector3& scaling, const Vector3& world_pivot ){
	if ( GlobalSelectionSystem().countSelectedComponents() != 0 ) {
		GlobalSelectionSystem().foreachSelectedComponent( scale_component_selected( scaling, world_pivot ) );
	}
}

class skew_component_selected : public SelectionSystem::Visitor
{
	const Skew& m_skew;
	const Vector3& m_world_pivot;
public:
	skew_component_selected( const Skew& skew, const Vector3& world_pivot )
		: m_skew( skew ), m_world_pivot( world_pivot ){
	}
	void visit( scene::Instance& instance ) const override {
		Transformable* transform = Instance_getTransformable( instance );
		if ( transform != 0 ) {
			Vector3 parent_translation;
			translation_for_pivoted_skew( parent_translation, m_skew, m_world_pivot, instance.localToWorld(), Node_getTransformNode( instance.path().top() )->localToParent() );

			transform->setType( TRANSFORM_COMPONENT );
			transform->setSkew( m_skew );
			transform->setTranslation( parent_translation );
		}
	}
};

void Scene_Skew_Component_Selected( scene::Graph& graph, const Skew& skew, const Vector3& world_pivot ){
	if ( GlobalSelectionSystem().countSelectedComponents() != 0 ) {
		GlobalSelectionSystem().foreachSelectedComponent( skew_component_selected( skew, world_pivot ) );
	}
}


class transform_component_selected : public SelectionSystem::Visitor
{
	const Transforms& m_transforms;
	const Vector3& m_world_pivot;
public:
	transform_component_selected( const Transforms& transforms, const Vector3& world_pivot )
		: m_transforms( transforms ), m_world_pivot( world_pivot ){
	}
	void visit( scene::Instance& instance ) const override {
		Transformable* transform = Instance_getTransformable( instance );
		if ( transform != 0 ) {
			const Matrix4 local_transform = matrix4_transform_for_components( c_translation_identity, m_transforms.getRotation(), m_transforms.getScale(), m_transforms.getSkew() );
			Vector3 parent_translation;
			translation_for_pivoted_matrix_transform( parent_translation, local_transform, m_world_pivot, instance.localToWorld(), Node_getTransformNode( instance.path().top() )->localToParent() );

			transform->setType( TRANSFORM_COMPONENT );
			transform->setRotation( m_transforms.getRotation() );
			transform->setScale( m_transforms.getScale() );
			transform->setSkew( m_transforms.getSkew() );
			transform->setTranslation( parent_translation + m_transforms.getTranslation() );
		}
	}
};


class BooleanSelector : public Selector
{
	SelectionIntersection m_bestIntersection;
	Selectable* m_selectable;
public:
	BooleanSelector() : m_bestIntersection( SelectionIntersection() ){
	}

	void pushSelectable( Selectable& selectable ) override {
		m_selectable = &selectable;
	}
	void popSelectable() override {
	}
	void addIntersection( const SelectionIntersection& intersection ) override {
		if ( m_selectable->isSelected() ) {
			assign_if_closer( m_bestIntersection, intersection );
		}
	}

	bool isSelected(){
		return m_bestIntersection.valid();
	}
	const SelectionIntersection& bestIntersection() const {
		return m_bestIntersection;
	}
};

class BestSelector : public Selector
{
protected:
	SelectionIntersection m_intersection;
	Selectable* m_selectable;
	SelectionIntersection m_bestIntersection;
	std::list<Selectable*> m_bestSelectable;
public:
	BestSelector() : m_bestIntersection( SelectionIntersection() ), m_bestSelectable( 0 ){
	}

	void pushSelectable( Selectable& selectable ) override {
		m_intersection = SelectionIntersection();
		m_selectable = &selectable;
	}
	void popSelectable() override {
		if ( m_intersection.equalEpsilon( m_bestIntersection, 0.25f, 2e-6f ) ) {
			m_bestSelectable.push_back( m_selectable );
			m_bestIntersection = m_intersection;
		}
		else if ( m_intersection < m_bestIntersection ) {
			m_bestSelectable.clear();
			m_bestSelectable.push_back( m_selectable );
			m_bestIntersection = m_intersection;
		}
		m_intersection = SelectionIntersection();
	}
	void addIntersection( const SelectionIntersection& intersection ) override {
		assign_if_closer( m_intersection, intersection );
	}

	std::list<Selectable*>& best(){
		return m_bestSelectable;
	}
	const SelectionIntersection& bestIntersection() const {
		return m_bestIntersection;
	}
};

class DeepBestSelector : public BestSelector // copy of class BestSelector with 2.f depthEpsilon
{
public:
	void popSelectable() override {
		if ( m_intersection.equalEpsilon( m_bestIntersection, 0.25f, 2.f ) ) {
			m_bestSelectable.push_back( m_selectable );
			m_bestIntersection = m_intersection;
		}
		else if ( m_intersection < m_bestIntersection ) {
			m_bestSelectable.clear();
			m_bestSelectable.push_back( m_selectable );
			m_bestIntersection = m_intersection;
		}
		m_intersection = SelectionIntersection();
	}
};

class BestPointSelector : public Selector
{
	SelectionIntersection m_bestIntersection;
public:
	BestPointSelector() : m_bestIntersection( SelectionIntersection() ){
	}

	void pushSelectable( Selectable& selectable ) override {
	}
	void popSelectable() override {
	}
	void addIntersection( const SelectionIntersection& intersection ) override {
		assign_if_closer( m_bestIntersection, intersection );
	}

	bool isSelected(){
		return m_bestIntersection.valid();
	}
	const SelectionIntersection& best() const {
		return m_bestIntersection;
	}
};





class ScenePointSelector : public Selector {
	SelectionIntersection m_bestIntersection;
	Face* m_face;
public:
	ScenePointSelector() : m_bestIntersection( SelectionIntersection() ), m_face( 0 ) {
	}

	void pushSelectable( Selectable& selectable ) override {
	}
	void popSelectable() override {
	}
	void addIntersection( const SelectionIntersection& intersection ) override {
		if( SelectionIntersection_closer( intersection, m_bestIntersection ) ) {
			m_bestIntersection = intersection;
			m_face = 0;
		}
	}

	void addIntersection( const SelectionIntersection& intersection, Face* face ) {
		if( SelectionIntersection_closer( intersection, m_bestIntersection ) ) {
			m_bestIntersection = intersection;
			m_face = face;
		}
	}
	bool isSelected() {
		return m_bestIntersection.valid();
	}
	const SelectionIntersection& best() {
		return m_bestIntersection;
	}
	const Face* face() {
		return m_face;
	}
};

namespace detail
{
inline void testselect_scene_point__brush( BrushInstance* brush, ScenePointSelector& m_selector, SelectionTest& m_test ){
	m_test.BeginMesh( brush->localToWorld() );
	for( Brush::const_iterator i = brush->getBrush().begin(); i != brush->getBrush().end(); ++i ) {
		Face* face = *i;
		if( !face->isFiltered() ) {
			SelectionIntersection intersection;
			face->testSelect( m_test, intersection );
			m_selector.addIntersection( intersection, face );
		}
	}
}
}
class testselect_scene_point : public scene::Graph::Walker {
	ScenePointSelector& m_selector;
	SelectionTest& m_test;
public:
	testselect_scene_point( ScenePointSelector& selector, SelectionTest& test ) : m_selector( selector ), m_test( test ) {
	}
	bool pre( const scene::Path& path, scene::Instance& instance ) const override {
		if( BrushInstance* brush = Instance_getBrush( instance ) ) {
			detail::testselect_scene_point__brush( brush, m_selector, m_test );
		}
		else if( SelectionTestable* selectionTestable = Instance_getSelectionTestable( instance ) ) {
			selectionTestable->testSelect( m_selector, m_test );
		}
		return true;
	}
};

class testselect_scene_point_unselected : public scene::Graph::Walker {
	ScenePointSelector& m_selector;
	SelectionTest& m_test;
public:
	testselect_scene_point_unselected( ScenePointSelector& selector, SelectionTest& test ) : m_selector( selector ), m_test( test ) {
	}
	bool pre( const scene::Path& path, scene::Instance& instance ) const override {
		if( !Instance_isSelected( instance ) ){
			if( BrushInstance* brush = Instance_getBrush( instance ) ) {
				detail::testselect_scene_point__brush( brush, m_selector, m_test );
			}
			else if( SelectionTestable* selectionTestable = Instance_getSelectionTestable( instance ) ) {
				selectionTestable->testSelect( m_selector, m_test );
			}
			return true;
		}
		return false; // avoids entities with node unselected (e.g. model)
	}
};

class testselect_scene_point_selected_brushes : public scene::Graph::Walker {
	ScenePointSelector& m_selector;
	SelectionTest& m_test;
public:
	testselect_scene_point_selected_brushes( ScenePointSelector& selector, SelectionTest& test ) : m_selector( selector ), m_test( test ) {
	}
	bool pre( const scene::Path& path, scene::Instance& instance ) const override {
		if( Instance_isSelected( instance ) ){
			if( BrushInstance* brush = Instance_getBrush( instance ) ) {
				detail::testselect_scene_point__brush( brush, m_selector, m_test );
			}
		}
		return true;
	}
};

DoubleVector3 testSelected_scene_snapped_point( const SelectionVolume& test, ScenePointSelector& selector ){
	DoubleVector3 point = vector4_projected( matrix4_transformed_vector4( test.getScreen2world(), Vector4( 0, 0, selector.best().depth(), 1 ) ) );
	if( selector.face() ){
		const Face& face = *selector.face();
		double bestDist = FLT_MAX;
		DoubleVector3 wannabePoint;
		for ( Winding::const_iterator prev = face.getWinding().end() - 1, curr = face.getWinding().begin(); curr != face.getWinding().end(); prev = curr, ++curr ){
			const DoubleVector3 v1( prev->vertex );
			const DoubleVector3 v2( curr->vertex );
			{	/* try vertices */
				const double dist = vector3_length_squared( v2 - point );
				if( dist < bestDist ){
					wannabePoint = v2;
					bestDist = dist;
				}
			}
			{	/* try edges */
				DoubleVector3 edgePoint = line_closest_point( DoubleLine( v1, v2 ), point );
				if( edgePoint != v1 && edgePoint != v2 ){
					const DoubleVector3 edgedir = vector3_normalised( v2 - v1 );
					const std::size_t maxi = vector3_max_abs_component_index( edgedir );
					// v1[maxi] + edgedir[maxi] * coef = float_snapped( point[maxi], GetSnapGridSize() )
					const double coef = ( float_snapped( point[maxi], GetSnapGridSize() ) - v1[maxi] ) / edgedir[maxi];
					edgePoint = v1 + edgedir * coef;
					const double dist = vector3_length_squared( edgePoint - point );
					if( dist < bestDist ){
						wannabePoint = edgePoint;
						bestDist = dist;
					}
				}
			}
		}
		if( selector.best().distance() == 0.f ){ /* try plane, if pointing inside of polygon */
			const std::size_t maxi = vector3_max_abs_component_index( face.plane3().normal() );
			DoubleVector3 planePoint( vector3_snapped( point, GetSnapGridSize() ) );
			// face.plane3().normal().dot( point snapped ) = face.plane3().dist()
			planePoint[maxi] = ( face.plane3().dist()
			                     - face.plane3().normal()[( maxi + 1 ) % 3] * planePoint[( maxi + 1 ) % 3]
			                     - face.plane3().normal()[( maxi + 2 ) % 3] * planePoint[( maxi + 2 ) % 3] ) / face.plane3().normal()[maxi];
			const double dist = vector3_length_squared( planePoint - point );
			if( dist < bestDist ){
				wannabePoint = planePoint;
				bestDist = dist;
			}
		}
		point = wannabePoint;
	}
	else{
		vector3_snap( point, GetSnapGridSize() );
	}
	return point;
}

std::optional<testSelect_unselected_scene_point_return_t>
testSelect_unselected_scene_point( const View& view, const DeviceVector device_point, const DeviceVector device_epsilon ){
	View scissored( view );
	ConstructSelectionTest( scissored, SelectionBoxForPoint( device_point, device_epsilon ) );

	SelectionVolume test( scissored );
	ScenePointSelector selector;
	Scene_forEachVisible( GlobalSceneGraph(), scissored, testselect_scene_point_unselected( selector, test ) );
	test.BeginMesh( g_matrix4_identity, true );
	if( selector.isSelected() ){
		return testSelect_unselected_scene_point_return_t{ testSelected_scene_snapped_point( test, selector ),
			selector.face() != nullptr? selector.face()->plane3() : std::optional<Plane3>() };
	}
	return {};
}

std::optional<Vector3> AABB_TestPoint( const View& view, const DeviceVector device_point, const DeviceVector device_epsilon, const AABB& aabb ){
	View scissored( view );
	ConstructSelectionTest( scissored, SelectionBoxForPoint( device_point, device_epsilon ) );

	SelectionIntersection best;
	AABB_BestPoint( scissored.GetViewMatrix(), eClipCullCW, aabb, best );
	if( best.valid() ){
		return vector4_projected( matrix4_transformed_vector4( matrix4_full_inverse( scissored.GetViewMatrix() ), Vector4( 0, 0, best.depth(), 1 ) ) );
	}
	return {};
}

bool scene_insert_brush_vertices( const View& view, TranslateFreeXY_Z& freeDragXY_Z ){
	SelectionVolume test( view );
	ScenePointSelector selector;
	if( view.fill() )
		Scene_forEachVisible( GlobalSceneGraph(), view, testselect_scene_point( selector, test ) );
	else
		Scene_forEachVisible( GlobalSceneGraph(), view, testselect_scene_point_selected_brushes( selector, test ) );
	test.BeginMesh( g_matrix4_identity, true );
	if( selector.isSelected() ){
		freeDragXY_Z.set0( vector4_projected( matrix4_transformed_vector4( test.getScreen2world(), Vector4( 0, 0, selector.best().depth(), 1 ) ) ) );
		DoubleVector3 point = testSelected_scene_snapped_point( test, selector );
		if( !view.fill() ){
			point -= view.getViewDir() * GetGridSize();
		}
		Brush::VertexModeVertices vertexModeVertices;
		vertexModeVertices.push_back( Brush::VertexModeVertex( point, true ) );
		if( selector.face() )
			vertexModeVertices.back().m_faces.push_back( selector.face() );

		UndoableCommand undo( "InsertBrushVertices" );
		Scene_forEachSelectedBrush( [&vertexModeVertices]( BrushInstance& brush ){ brush.insert_vertices( vertexModeVertices ); } );
		return true;
	}
	else if( !view.fill() ){ //+two points
		freeDragXY_Z.set0( g_vector3_identity );
		const AABB bounds = GlobalSelectionSystem().getBoundsSelected();
		if( aabb_valid( bounds ) ){
			DoubleVector3 xy = vector4_projected( matrix4_transformed_vector4( test.getScreen2world(), Vector4( 0, 0, 0, 1 ) ) );
			vector3_snap( xy, GetSnapGridSize() );
			DoubleVector3 a( xy ), b( xy );
			const std::size_t max = vector3_max_abs_component_index( view.getViewDir() );
			a[max] = bounds.origin[max] + bounds.extents[max];
			b[max] = bounds.origin[max] - bounds.extents[max];
			Brush::VertexModeVertices vertexModeVertices;
			vertexModeVertices.push_back( Brush::VertexModeVertex( a, true ) );
			vertexModeVertices.push_back( Brush::VertexModeVertex( b, true ) );

			UndoableCommand undo( "InsertBrushVertices" );
			Scene_forEachSelectedBrush( [&vertexModeVertices]( BrushInstance& brush ){ brush.insert_vertices( vertexModeVertices ); } );
			return true;
		}
	}
	return false;
}

bool selection_selectVerticesOrFaceVertices( SelectionTest& test ){
	{	/* try to hit vertices */
		DeepBestSelector deepSelector;
		Scene_TestSelect_Component_Selected( deepSelector, test, test.getVolume(), SelectionSystem::eVertex );
		if( !deepSelector.best().empty() ){
			for ( Selectable* s : deepSelector.best() )
				s->setSelected( true );
			return true;
		}
	}
	/* otherwise select vertices of brush faces, which lay on best plane */
	const auto planeData = Scene_forEachSelectedBrush_bestPlane( test );

	if( planeData.valid() ){
		auto selectVerticesOnPlane = [plane = planeData.m_plane]( BrushInstance& brushInstance ){
			brushInstance.selectVerticesOnPlane( plane );
		};
		Scene_forEachVisibleSelectedBrush( selectVerticesOnPlane );
	}
	return planeData.valid();
}


template<typename Functor>
class ComponentSelectionTestableVisibleSelectedVisitor : public SelectionSystem::Visitor
{
	const Functor& m_functor;
public:
	ComponentSelectionTestableVisibleSelectedVisitor( const Functor& functor ) : m_functor( functor ){
	}
	void visit( scene::Instance& instance ) const override {
		ComponentSelectionTestable* componentSelectionTestable = Instance_getComponentSelectionTestable( instance );
		if ( componentSelectionTestable != 0
		  && instance.path().top().get().visible() ) {
			m_functor( *componentSelectionTestable );
		}
	}
};

template<typename Functor>
inline const Functor& Scene_forEachVisibleSelectedComponentSelectionTestable( const Functor& functor ){
	GlobalSelectionSystem().foreachSelected( ComponentSelectionTestableVisibleSelectedVisitor<Functor>( functor ) );
	return functor;
}



static bool g_bTmpComponentMode = false;

static bool g_3DCreateBrushes = true;

class DragManipulator : public Manipulator
{
	ResizeTranslatable m_resize;
	TranslateFree m_freeResize;
	TranslateAxis2 m_axisResize;
	TranslateFreeXY_Z m_freeDragXY_Z;
	DragNewBrush m_dragNewBrush;
	DragExtrudeFaces m_dragExtrudeFaces;
	bool m_dragSelected; //drag selected primitives or components
	bool m_selected; //components selected temporally for drag
	bool m_selected2; //planeselectables in cam with alt
	bool m_newBrush;
	bool m_extrudeFaces;

public:

	static Shader* m_state_wire;

	DragManipulator( Translatable& translatable, AllTransformable& transformable ) :
		m_resize(), m_freeResize( m_resize ), m_axisResize( m_resize ), m_freeDragXY_Z( translatable, transformable ), m_renderCircle( 2 << 3 ){
		setSelected( false );
		draw_circle( m_renderCircle.m_vertices.size() >> 3, 5, m_renderCircle.m_vertices.data(), RemapXYZ() );
	}

	Manipulatable* GetManipulatable() override {
		if( m_newBrush )
			return &m_dragNewBrush;
		else if( m_extrudeFaces )
			return &m_dragExtrudeFaces;
		else if( m_selected )
			return &m_freeResize;
		else if( m_selected2 )
			return &m_axisResize;
		else
			return &m_freeDragXY_Z;
	}

	void testSelect( const View& view, const Matrix4& pivot2world ) override {
		SelectionPool selector;
		SelectionVolume test( view );

		if( g_modifiers == ( c_modifierAlt | c_modifierControl )
		 && GlobalSelectionSystem().Mode() == SelectionSystem::ePrimitive
		 && ( GlobalSelectionSystem().countSelected() != 0 || !g_SelectedFaceInstances.empty() ) ){ // extrude
			m_extrudeFaces = Scene_forEachBrush_setupExtrude( test, m_dragExtrudeFaces );
		}
		else if( GlobalSelectionSystem().countSelected() != 0 ){
			if ( GlobalSelectionSystem().Mode() == SelectionSystem::ePrimitive ){
				if( g_modifiers == c_modifierAlt ){
					if( view.fill() ){ // alt resize
						m_selected2 = Scene_forEachPlaneSelectable_selectPlanes2( test, m_axisResize );
					}
					else{ // alt vertices drag
						m_selected = selection_selectVerticesOrFaceVertices( test );
					}
				}
				else if( g_modifiers == c_modifierNone ){
					BooleanSelector booleanSelector;
					Scene_TestSelect_Primitive( booleanSelector, test, view );

					if ( booleanSelector.isSelected() ) { /* hit a primitive */
						m_dragSelected = true; /* drag a primitive */
						test.BeginMesh( g_matrix4_identity, true );
						m_freeDragXY_Z.set0( vector4_projected( matrix4_transformed_vector4( test.getScreen2world(), Vector4( 0, 0, booleanSelector.bestIntersection().depth(), 1 ) ) ) );
					}
					else{ /* haven't hit a primitive */
						m_selected = Scene_forEachPlaneSelectable_selectPlanes( GlobalSceneGraph(), selector, test ); /* select faces on planeSelectables */
					}
				}
			}
			else if( g_modifiers == c_modifierNone ){ // components
				BestSelector bestSelector;
				Scene_TestSelect_Component_Selected( bestSelector, test, view, GlobalSelectionSystem().ComponentMode() ); /* drag components */
				for ( Selectable* s : bestSelector.best() ){
					if ( !s->isSelected() )
						GlobalSelectionSystem().setSelectedAllComponents( false );
					selector.addSelectable( SelectionIntersection( 0, 0 ), s );
					m_dragSelected = true;
				}
				if( bestSelector.bestIntersection().valid() ){
					test.BeginMesh( g_matrix4_identity, true );
					m_freeDragXY_Z.set0( vector4_projected( matrix4_transformed_vector4( test.getScreen2world(), Vector4( 0, 0, bestSelector.bestIntersection().depth(), 1 ) ) ) );
				}
				else{
					if( GlobalSelectionSystem().countSelectedComponents() != 0 ){ /* drag, even if hit nothing, but got selected */
						m_dragSelected = true;
						m_freeDragXY_Z.set0( g_vector3_identity );
					}
					else if( GlobalSelectionSystem().ComponentMode() == SelectionSystem::eVertex ){ /* otherwise insert */
						m_dragSelected = g_bTmpComponentMode = scene_insert_brush_vertices( view, m_freeDragXY_Z ); //hack: indicating not a tmp mode
						return;
					}
				}
			}

			for ( SelectableSortedSet::value_type& value : selector )
				value.second->setSelected( true );
			g_bTmpComponentMode = m_selected | m_selected2;
		}
		else if( GlobalSelectionSystem().Mode() == SelectionSystem::ePrimitive && g_3DCreateBrushes && g_modifiers == c_modifierNone ){
			m_newBrush = true;
			BestPointSelector bestPointSelector;
			Scene_TestSelect_Primitive( bestPointSelector, test, view );
			Vector3 start;
			test.BeginMesh( g_matrix4_identity, true );
			if( bestPointSelector.isSelected() ){
				start = vector4_projected( matrix4_transformed_vector4( test.getScreen2world(), Vector4( 0, 0, bestPointSelector.best().depth(), 1 ) ) );
			}
			else{
				const Vector3 pnear = vector4_projected( matrix4_transformed_vector4( test.getScreen2world(), Vector4( 0, 0, -1, 1 ) ) );
				const Vector3 pfar = vector4_projected( matrix4_transformed_vector4( test.getScreen2world(), Vector4( 0, 0, 1, 1 ) ) );
				start = vector3_normalised( pfar - pnear ) * ( 256.f + GetGridSize() * sqrt( 3.0 ) ) + pnear;
			}
			vector3_snap( start, GetSnapGridSize() );
			m_dragNewBrush.set0( start );
		}
	}

	void setSelected( bool select ) override {
		m_dragSelected = select;
		m_selected = select;
		m_selected2 = select;
		m_newBrush = select;
		m_extrudeFaces = select;
	}
	bool isSelected() const override {
		return m_dragSelected || m_selected || m_selected2 || m_newBrush || m_extrudeFaces;
	}

	void render( Renderer& renderer, const VolumeTest& volume, const Matrix4& pivot2world ) override {
		if( !m_polygons.empty() ){
			renderer.SetState( m_state_wire, Renderer::eWireframeOnly );
			renderer.SetState( m_state_wire, Renderer::eFullMaterials );
			if( m_polygons.back().size() == 1 ){
				Pivot2World_viewplaneSpace( m_renderCircle.m_viewplaneSpace, matrix4_translation_for_vec3( m_polygons.back()[0] ), volume.GetModelview(), volume.GetProjection(), volume.GetViewport() );
				renderer.addRenderable( m_renderCircle, m_renderCircle.m_viewplaneSpace );
			}
			else{
				renderer.addRenderable( m_renderPoly, g_matrix4_identity );
			}
		}
	}
	void highlight( const View& view ){
		SelectionVolume test( view );
		std::vector<std::vector<Vector3>> polygons;
		/* conditions structure respects one in testSelect() */
		if( g_modifiers == ( c_modifierAlt | c_modifierControl )
		 && GlobalSelectionSystem().Mode() == SelectionSystem::ePrimitive
		 && ( GlobalSelectionSystem().countSelected() != 0 || !g_SelectedFaceInstances.empty() ) ){ // extrude
			if( const auto planeData = Scene_forEachBrush_bestPlane( test ); planeData.valid() ){
				auto gatherPolygonsByPlane = [plane = planeData.m_plane, &polygons]( BrushInstance& brushInstance ){
					if( brushInstance.isSelected() || brushInstance.isSelectedComponents() )
						brushInstance.gatherPolygonsByPlane( plane, polygons, false );
				};
				Scene_forEachVisibleBrush( GlobalSceneGraph(), gatherPolygonsByPlane );
			}
		}
		else if( GlobalSelectionSystem().countSelected() != 0 ){
			if ( GlobalSelectionSystem().Mode() == SelectionSystem::ePrimitive ){
				if( g_modifiers == c_modifierAlt ){
					if( view.fill() ){ // alt resize
						if( const auto planeData = Scene_forEachPlaneSelectable_bestPlane( test ); planeData.valid() ){
							auto gatherPolygonsByPlane = [plane = planeData.m_plane, &polygons]( PlaneSelectable& planeSelectable ){
								planeSelectable.gatherPolygonsByPlane( plane, polygons );
							};
							Scene_forEachVisibleSelectedPlaneselectable( gatherPolygonsByPlane );
						}

					}
					else{ // alt vertices drag
						SelectionIntersection intersection;
						const SelectionSystem::EComponentMode mode = SelectionSystem::eVertex;
						auto gatherComponentsHighlight = [&polygons, &intersection, &test, mode]( const ComponentSelectionTestable& componentSelectionTestable ){
							componentSelectionTestable.gatherComponentsHighlight( polygons, intersection, test, mode );
						};
						Scene_forEachVisibleSelectedComponentSelectionTestable( gatherComponentsHighlight );

						if( polygons.empty() ){
							if( const auto planeData = Scene_forEachSelectedBrush_bestPlane( test ); planeData.valid() ){
								auto gatherPolygonsByPlane = [plane = planeData.m_plane, &polygons]( BrushInstance& brushInstance ){
									brushInstance.gatherPolygonsByPlane( plane, polygons );
								};
								Scene_forEachVisibleSelectedBrush( gatherPolygonsByPlane );
							}
						}
					}
				}
			}
			else if( g_modifiers == c_modifierNone // components
				|| g_modifiers == c_modifierShift // hack: these respect to the RadiantSelectionSystem::SelectPoint
				|| ( g_modifiers == c_modifierControl && GlobalSelectionSystem().ComponentMode() == SelectionSystem::EComponentMode::eFace ) ){
				SelectionIntersection intersection;
				const SelectionSystem::EComponentMode mode = GlobalSelectionSystem().ComponentMode();
				auto gatherComponentsHighlight = [&polygons, &intersection, &test, mode]( const ComponentSelectionTestable& componentSelectionTestable ){
					componentSelectionTestable.gatherComponentsHighlight( polygons, intersection, test, mode );
				};
				Scene_forEachVisibleSelectedComponentSelectionTestable( gatherComponentsHighlight );
			}
		}

		if( m_polygons != polygons ){
			m_polygons.swap( polygons );
			SceneChangeNotify();
		}
	}
private:
	std::vector<std::vector<Vector3>> m_polygons;
	struct RenderablePoly: public OpenGLRenderable
	{
		const std::vector<std::vector<Vector3>>& m_polygons;

		RenderablePoly( const std::vector<std::vector<Vector3>>& polygons ) : m_polygons( polygons ){
		}
		void render( RenderStateFlags state ) const override {
			gl().glPolygonOffset( -2, -2 );
			for( const auto& poly : m_polygons ){
				gl().glVertexPointer( 3, GL_FLOAT, sizeof( m_polygons[0][0] ), poly[0].data() );
				gl().glDrawArrays( GL_POLYGON, 0, GLsizei( poly.size() ) );
			}
			gl().glPolygonOffset( -1, 1 ); // restore default
		}
	};
	RenderablePoly m_renderPoly{ m_polygons };
	struct RenderableCircle : public OpenGLRenderable
	{
		Array<PointVertex> m_vertices;
		Matrix4 m_viewplaneSpace;

		RenderableCircle( std::size_t size ) : m_vertices( size ){
		}
		void render( RenderStateFlags state ) const override {
			gl().glVertexPointer( 3, GL_FLOAT, sizeof( PointVertex ), &m_vertices.data()->vertex );
			gl().glDrawArrays( GL_LINE_LOOP, 0, GLsizei( m_vertices.size() ) );
		}
	};
	RenderableCircle m_renderCircle;
};
Shader* DragManipulator::m_state_wire;



#include "clippertool.h"

class ClipManipulator : public Manipulator, public ManipulatorSelectionChangeable, public Translatable, public AllTransformable, public Manipulatable
{
	struct ClipperPoint : public OpenGLRenderable, public SelectableBool
	{
		PointVertex m_p; //for render
		ClipperPoint():
			m_p( vertex3f_identity ), m_set( false ) {
		}
		void render( RenderStateFlags state ) const override {
			gl().glColorPointer( 4, GL_UNSIGNED_BYTE, sizeof( PointVertex ), &m_p.colour );
			gl().glVertexPointer( 3, GL_FLOAT, sizeof( PointVertex ), &m_p.vertex );
			gl().glDrawArrays( GL_POINTS, 0, 1 );

			gl().glColor4ub( m_p.colour.r, m_p.colour.g, m_p.colour.b, m_p.colour.a ); ///?
			gl().glRasterPos3f( m_namePos.x(), m_namePos.y(), m_namePos.z() );
			GlobalOpenGL().drawChar( m_name );
		}
		void setColour( const Colour4b& colour ) {
			m_p.colour = colour;
		}
		bool m_set;
		DoubleVector3 m_point;
		DoubleVector3 m_pointNonTransformed;
		char m_name;
		Vector3 m_namePos;
	};
	Matrix4& m_pivot2world;
	ClipperPoint m_points[3];
	TranslateFreeXY_Z m_dragXY_Z;
	const AABB& m_bounds;
	Vector3 m_viewdir;
public:
	static Shader* m_state;

	ClipManipulator( Matrix4& pivot2world, const AABB& bounds ) : m_pivot2world( pivot2world ), m_dragXY_Z( *this, *this ), m_bounds( bounds ){
		m_points[0].m_name = '1';
		m_points[1].m_name = '2';
		m_points[2].m_name = '3';
	}

	void UpdateColours() {
		for( std::size_t i = 0; i < 3; ++i )
			m_points[i].setColour( colourSelected( g_colour_screen, m_points[i].isSelected() ) );
	}

	void render( Renderer& renderer, const VolumeTest& volume, const Matrix4& pivot2world ) override {
		// temp hack
		UpdateColours();

		renderer.SetState( m_state, Renderer::eWireframeOnly );
		renderer.SetState( m_state, Renderer::eFullMaterials );

		const Matrix4 proj( matrix4_multiplied_by_matrix4( volume.GetViewport(), volume.GetViewMatrix() ) );
		const Matrix4 proj_inv( matrix4_full_inverse( proj ) );
		for( std::size_t i = 0; i < 3; ++i )
			if( m_points[i].m_set ){
				m_points[i].m_p.vertex = vertex3f_for_vector3( m_points[i].m_point );
				renderer.addRenderable( m_points[i], g_matrix4_identity );
				const Vector3 pos = vector4_projected( matrix4_transformed_vector4( proj, Vector4( m_points[i].m_point, 1 ) ) ) + Vector3( 2, 0, 0 );
				m_points[i].m_namePos = vector4_projected( matrix4_transformed_vector4( proj_inv, Vector4( pos, 1 ) ) );
			}
	}
	/* these three functions and m_viewdir for 2 points only */
	void viewdir_set( const Vector3 viewdir ){
		const std::size_t maxi = vector3_max_abs_component_index( viewdir );
		m_viewdir = ( viewdir[maxi] > 0 )? g_vector3_axes[maxi] : -g_vector3_axes[maxi];
	}
	void viewdir_fixup(){
		if( fabs( vector3_length( m_points[1].m_point - m_points[0].m_point ) ) > 1e-3 //two non coincident points
		 && fabs( vector3_dot( m_viewdir, vector3_normalised( m_points[1].m_point - m_points[0].m_point ) ) ) > 0.999 ){ //on axis = m_viewdir
			viewdir_set( m_view->getViewDir() );
			if( fabs( vector3_dot( m_viewdir, vector3_normalised( m_points[1].m_point - m_points[0].m_point ) ) ) > 0.999 ){
				const Matrix4 screen2world( matrix4_full_inverse( m_view->GetViewMatrix() ) );
				Vector3 p[2];
				for( std::size_t i = 0; i < 2; ++i ){
					p[i] = vector4_projected( matrix4_transformed_vector4( m_view->GetViewMatrix(), Vector4( m_points[i].m_point, 1 ) ) );
				}
				const float depthdir = p[1].z() > p[0].z()? -1 : 1;
				for( std::size_t i = 0; i < 2; ++i ){
					p[i].z() = -1;
					p[i] = vector4_projected( matrix4_transformed_vector4( screen2world, Vector4( p[i], 1 ) ) );
				}
				viewdir_set( ( p[1] - p[0] ) * depthdir );
			}
		}
	}
	void viewdir_make_cut_worthy( const Plane3& plane ){
		const std::size_t maxi = vector3_max_abs_component_index( plane.normal() );
		if( plane3_valid( plane )
		 && aabb_valid( m_bounds )
		 && fabs( plane.normal()[maxi] ) > 0.999 ){ //axial plane
			const double anchor = plane.normal()[maxi] * plane.dist();
			if( anchor > m_bounds.origin[maxi] ){
				if( ( anchor - ( m_bounds.origin[maxi] + m_bounds.extents[maxi] ) ) > -0.1 )
					viewdir_set( -g_vector3_axes[maxi] );
			}
			else{
				if( ( -anchor + ( m_bounds.origin[maxi] - m_bounds.extents[maxi] ) ) > -0.1 )
					viewdir_set( g_vector3_axes[maxi] );
			}
		}
	}
	void updatePlane(){
		std::size_t npoints = 0;
		for(; npoints < 3; )
			if( m_points[npoints].m_set )
				++npoints;
			else
				break;

		switch ( npoints )
		{
		case 1:
			Clipper_setPlanePoints( ClipperPoints( m_points[0].m_point, m_points[0].m_point, m_points[0].m_point, npoints ) );
			break;
		case 2:
			{
				if( m_view->fill() ){ //3d
					viewdir_fixup();
					m_points[2].m_point = m_points[0].m_point - m_viewdir * vector3_length( m_points[0].m_point - m_points[1].m_point );
					viewdir_make_cut_worthy( plane3_for_points( m_points[0].m_point, m_points[1].m_point, m_points[2].m_point ) );
				}
				m_points[2].m_point = m_points[0].m_point - m_viewdir * vector3_length( m_points[0].m_point - m_points[1].m_point );
			} // fall through
		case 3:
			Clipper_setPlanePoints( ClipperPoints( m_points[0].m_point, m_points[1].m_point, m_points[2].m_point, npoints ) );
			break;

		default:
			Clipper_setPlanePoints( ClipperPoints() );
			break;
		}
	}
	std::size_t newPointIndex( bool viewfill ) const {
		const std::size_t maxi = ( !viewfill && Clipper_get2pointsIn2d() )? 2 : 3;
		std::size_t i;
		for( i = 0; i < maxi; ++i )
			if( !m_points[i].m_set )
				break;
		return i % maxi;
	}
	void newPoint( const DoubleVector3& point, const View& view ){
		const std::size_t i = newPointIndex( view.fill() );
		if( i == 0 )
			m_points[1].m_set = m_points[2].m_set = false;
		m_points[i].m_set = true;
		m_points[i].m_point = point;

		SelectionPool selector;
		selector.addSelectable( SelectionIntersection( 0, 0 ), &m_points[i] );
		selectionChange( selector );

		if( i == 1 )
			viewdir_set( m_view->getViewDir() );

		updatePlane();
	}
	bool testSelect_scene( const View& view, DoubleVector3& point ) const {
		SelectionVolume test( view );
		ScenePointSelector selector;
		Scene_forEachVisible( GlobalSceneGraph(), view, testselect_scene_point( selector, test ) );
		test.BeginMesh( g_matrix4_identity, true );
		if( selector.isSelected() ){
			point = testSelected_scene_snapped_point( test, selector );
			return true;
		}
		return false;
	}
	void testSelect( const View& view, const Matrix4& pivot2world ) override {
		if( g_modifiers != c_modifierNone && !quickCondition( g_modifiers, view ) )
			return selectionChange( nullptr );

		testSelect_points( view );
		if( !isSelected() ){
			if( view.fill() ){
				DoubleVector3 point;
				if( testSelect_scene( view, point ) )
					newPoint( point, view );
			}
			else{
				DoubleVector3 point = vector4_projected( matrix4_transformed_vector4( matrix4_full_inverse( view.GetViewMatrix() ), Vector4( 0, 0, 0, 1 ) ) );
				vector3_snap( point, GetSnapGridSize() );
				{
					const std::size_t maxi = vector3_max_abs_component_index( view.getViewDir() );
					const std::size_t i = newPointIndex( false );
					point[maxi] = m_bounds.origin[maxi] + ( i == 2? -1 : 1 ) * m_bounds.extents[maxi];
				}
				newPoint( point, view );
			}
		}
		for( std::size_t i = 0; i < 3; ++i )
			if( m_points[i].isSelected() ){
				m_points[i].m_pointNonTransformed = m_points[i].m_point;
				m_pivot2world = matrix4_translation_for_vec3( m_points[i].m_pointNonTransformed );
				break;
			}
	}
	void testSelect_points( const View& view ) {
		if( g_modifiers != c_modifierNone && !quickCondition( g_modifiers, view ) )
			return selectionChange( nullptr );

		SelectionPool selector;
		{
			const Matrix4 local2view( view.GetViewMatrix() );

			for( std::size_t i = 0; i < 3; ++i ){
				if( m_points[i].m_set ){
					SelectionIntersection best;
					Point_BestPoint( local2view, PointVertex( vertex3f_for_vector3( m_points[i].m_point ) ), best );
					selector.addSelectable( best, &m_points[i] );
				}
			}
		}
		selectionChange( selector );
	}
	void reset( bool initFromFace ){
		for( std::size_t i = 0; i < 3; ++i ){
			m_points[i].m_set = false;
			m_points[i].setSelected( false ); ///?
		}
		if( initFromFace && !g_SelectedFaceInstances.empty() && g_SelectedFaceInstances.last().getFace().contributes() ){
			const Winding& w = g_SelectedFaceInstances.last().getFace().getWinding();
			for( std::size_t i = 0; i < 3; ++i ){
				m_points[i].m_set = true;
				m_points[i].m_point = w[i].vertex;
			}
		}
		updatePlane();
	}
	/* Translatable */
	void translate( const Vector3& translation ) override { //in 2d and ( 3d + m_dragXY_Z )
		for( std::size_t i = 0; i < 3; ++i )
			if( m_points[i].isSelected() ){
				m_points[i].m_point = m_points[i].m_pointNonTransformed + translation;
				updatePlane();
				break;
			}
	}
	/* AllTransformable */
	void alltransform( const Transforms& transforms, const Vector3& world_pivot ) override {
		ERROR_MESSAGE( "unreachable" );
	}
	/* Manipulatable */
	void Construct( const Matrix4& device2manip, const DeviceVector device_point, const AABB& bounds, const Vector3& transform_origin ) override {
		m_dragXY_Z.set0( transform_origin );
		m_dragXY_Z.Construct( device2manip, device_point, AABB( transform_origin, g_vector3_identity ), transform_origin );
	}
	void Transform( const Matrix4& manip2object, const Matrix4& device2manip, const DeviceVector device_point ) override {
		// any 2D or 3D with modifiers besides SnapBounds
		if( !( g_modifiers == c_modifierNone && m_view->fill() ) && !SnapBounds::useCondition( g_modifiers, *m_view ) )
			return m_dragXY_Z.Transform( manip2object, device2manip, device_point );

		View scissored( *m_view );
		ConstructSelectionTest( scissored, SelectionBoxForPoint( device_point, m_device_epsilon ) );

		DoubleVector3 point;
		if( testSelect_scene( scissored, point ) )
			for( std::size_t i = 0; i < 3; ++i )
				if( m_points[i].isSelected() ){
					m_points[i].m_point = point;
					updatePlane();
					break;
				}
	}

	Manipulatable* GetManipulatable() override {
		return this;
	}

	void setSelected( bool select ) override {
		for( std::size_t i = 0; i < 3; ++i )
			m_points[i].setSelected( select );
	}
	bool isSelected() const override {
		return m_points[0].isSelected() || m_points[1].isSelected() || m_points[2].isSelected();
	}

	static bool quickCondition( const ModifierFlags& modifiers, const View& view ){
		return modifiers == c_modifierControl && !view.fill();
	}
};
Shader* ClipManipulator::m_state;




class BuildManipulator : public Manipulator, public Manipulatable
{
	struct RenderableLine : public OpenGLRenderable {
		PointVertex m_line[2];

		RenderableLine() {
		}
		void render( RenderStateFlags state ) const override {
			gl().glColorPointer( 4, GL_UNSIGNED_BYTE, sizeof( PointVertex ), &m_line[0].colour );
			gl().glVertexPointer( 3, GL_FLOAT, sizeof( PointVertex ), &m_line[0].vertex );
			gl().glDrawArrays( GL_LINES, 0, 2 );
		}
		void setColour( const Colour4b& colour ) {
			m_line[0].colour = colour;
			m_line[1].colour = colour;
		}
	};
	struct RenderablePoint : public OpenGLRenderable
	{
		PointVertex m_point;
		RenderablePoint():
			m_point( vertex3f_identity ) {
		}
		void render( RenderStateFlags state ) const override {
			gl().glColorPointer( 4, GL_UNSIGNED_BYTE, sizeof( PointVertex ), &m_point.colour );
			gl().glVertexPointer( 3, GL_FLOAT, sizeof( PointVertex ), &m_point.vertex );
			gl().glDrawArrays( GL_POINTS, 0, 1 );
		}
		void setColour( const Colour4b & colour ) {
			m_point.colour = colour;
		}
	};
	bool m_isSelected;
	bool m_isInitialised;
	RenderablePoint m_point;
	RenderableLine m_line;
	RenderableLine m_midline;
public:
	static Shader* m_state_point;
	static Shader* m_state_line;

	BuildManipulator() : m_isSelected( false ), m_isInitialised( false ) {
		m_point.setColour( g_colour_selected );
		m_line.setColour( g_colour_selected );
		m_midline.setColour( g_colour_screen );
	}
	void render( Renderer& renderer, const VolumeTest& volume, const Matrix4& pivot2world ) override {
		renderer.SetState( m_state_point, Renderer::eWireframeOnly );
		renderer.SetState( m_state_point, Renderer::eFullMaterials );
		renderer.addRenderable( m_point, g_matrix4_identity );
		renderer.SetState( m_state_line, Renderer::eWireframeOnly );
		renderer.SetState( m_state_line, Renderer::eFullMaterials );
		renderer.addRenderable( m_line, g_matrix4_identity );
		renderer.addRenderable( m_midline, g_matrix4_identity );
	}
	void initialise(){
	}
	void highlight( const View& view ){
		SceneChangeNotify();
	}

	void testSelect( const View& view, const Matrix4& pivot2world ) override {
		m_isSelected = true;
	}
	/* Manipulatable */
	void Construct( const Matrix4& device2manip, const DeviceVector device_point, const AABB& bounds, const Vector3& transform_origin ) override {
		//do things with undo
	}
	void Transform( const Matrix4& manip2object, const Matrix4& device2manip, const DeviceVector device_point ) override {
	}

	Manipulatable* GetManipulatable() override {
		m_isSelected = false; //don't handle the manipulator move part void MoveSelected()
		return this;
	}

	void setSelected( bool select ) override {
		m_isSelected = select;
	}
	bool isSelected() const override {
		return m_isSelected;
	}
};
Shader* BuildManipulator::m_state_point;
Shader* BuildManipulator::m_state_line;




#include "patch.h"
#include "iglrender.h"

class UVManipulator : public Manipulator, public Manipulatable
{
	struct RenderablePoint : public OpenGLRenderable
	{
		PointVertex m_point;
		RenderablePoint():
			m_point( vertex3f_identity ) {
		}
		void render( RenderStateFlags state ) const override {
			gl().glColorPointer( 4, GL_UNSIGNED_BYTE, sizeof( PointVertex ), &m_point.colour );
			gl().glVertexPointer( 3, GL_FLOAT, sizeof( PointVertex ), &m_point.vertex );
			gl().glDrawArrays( GL_POINTS, 0, 1 );
		}
		void setColour( const Colour4b & colour ) {
			m_point.colour = colour;
		}
	};
	struct RenderablePoints : public OpenGLRenderable
	{
		std::vector<PointVertex> m_points;
		RenderablePoints(){
		}
		void render( RenderStateFlags state ) const override {
			gl().glColorPointer( 4, GL_UNSIGNED_BYTE, sizeof( PointVertex ), &m_points[0].colour );
			gl().glVertexPointer( 3, GL_FLOAT, sizeof( PointVertex ), &m_points[0].vertex );
			gl().glDrawArrays( GL_POINTS, 0, m_points.size() );
		}
	};
	struct RenderableLines : public OpenGLRenderable
	{
		std::vector<PointVertex> m_lines;
		RenderableLines(){
		}
		void render( RenderStateFlags state ) const override {
			if( m_lines.size() != 0 ){
				gl().glColorPointer( 4, GL_UNSIGNED_BYTE, sizeof( PointVertex ), &m_lines[0].colour );
				gl().glVertexPointer( 3, GL_FLOAT, sizeof( PointVertex ), &m_lines[0].vertex );
				gl().glDrawArrays( GL_LINES, 0, m_lines.size() );
			}
		}
	};
	struct RenderableCircle : public OpenGLRenderable
	{
		Array<PointVertex> m_vertices;

		RenderableCircle( std::size_t size ) : m_vertices( size ){
		}
		void render( RenderStateFlags state ) const override {
			gl().glColorPointer( 4, GL_UNSIGNED_BYTE, sizeof( PointVertex ), &m_vertices.data()->colour );
			gl().glVertexPointer( 3, GL_FLOAT, sizeof( PointVertex ), &m_vertices.data()->vertex );
			gl().glDrawArrays( GL_LINE_LOOP, 0, GLsizei( m_vertices.size() ) );
		}
		void setColour( const Colour4b& colour ){
			for ( Array<PointVertex>::iterator i = m_vertices.begin(); i != m_vertices.end(); ++i )
			{
				( *i ).colour = colour;
			}
		}
	};
	typedef Array<PatchControl> PatchControlArray;
	struct RenderablePatchTexture : public OpenGLRenderable
	{
		std::vector<RenderIndex> m_trianglesIndices;
		const PatchControlArray* m_patchControlArray;
		RenderablePatchTexture(){
		}
		void render( RenderStateFlags state ) const override {
			if( state & RENDER_FILL ){
				const std::vector<Vector3> normals( m_patchControlArray->size(), g_vector3_axis_z );
				gl().glNormalPointer( GL_FLOAT, sizeof( Vector3 ), normals.data() );
				gl().glVertexPointer( 2, GL_FLOAT, sizeof( PatchControl ), &m_patchControlArray->data()->m_texcoord );
				gl().glTexCoordPointer( 2, GL_FLOAT, sizeof( PatchControl ), &m_patchControlArray->data()->m_texcoord );
				gl().glDrawElements( GL_TRIANGLES, GLsizei( m_trianglesIndices.size() ), RenderIndexTypeID, m_trianglesIndices.data() );
			}
		}
	};
	const Colour4b m_cWhite{ 255, 255, 255, 255 };
	const Colour4b m_cGray{ 255, 255, 255, 125 };
	const Colour4b m_cGrayer{ 100, 100, 100, 150 };
	const Colour4b m_cRed{ 255, 0, 0, 255 };
	const Colour4b m_cGreen{ 0, 255, 0, 255 };
	const Colour4b m_cGree{ 0, 150, 0, 255 };
	const Colour4b m_cPink{ 255, 0, 255, 255 };
	const Colour4b m_cPin{ 150, 0, 150, 255 };
	const Colour4b m_cOrange{ 255, 125, 0, 255 };
	const Colour4b m_cOrang{ 255, 125, 0, 125 };

	enum EUVSelection{
		eNone,
		ePivot,
		eGridU,
		eGridV,
		ePatchPoint,
		ePatchRow,
		ePatchColumn,
		eCircle,
		ePivotU,
		ePivotV,
		eU,
		eV,
		eUV,
		eSkewU,
		eSkewV,
		eTex,
	} m_selection;
	PointVertex* m_selectedU = 0; // must nullify this on m_Ulines, m_Vlines change
	PointVertex* m_selectedV = 0;
	int m_selectedPatchIndex = -1;
	bool m_isSelected = false;

	class UVSelector : public Selector {
		SelectionIntersection m_bestIntersection;
	public:
		EUVSelection m_selection = eNone;
		int m_index = -1;
		UVSelector() : m_bestIntersection( SelectionIntersection() ) {
		}
		void pushSelectable( Selectable& selectable ) override {
		}
		void popSelectable() override {
			m_bestIntersection = SelectionIntersection();
		}
		void addIntersection( const SelectionIntersection& intersection ) override {
			if( SelectionIntersection_closer( intersection, m_bestIntersection ) ) {
				m_bestIntersection = intersection;
			}
		}
		void addIntersection( const SelectionIntersection& intersection, EUVSelection selection, int index ) {
			if( SelectionIntersection_closer( intersection, m_bestIntersection ) ) {
				m_bestIntersection = intersection;
				m_selection = selection;
				m_index = index;
			}
		}
		void addIntersection( const SelectionIntersection& intersection, EUVSelection selection ) {
			if( SelectionIntersection_closer( intersection, m_bestIntersection ) ) {
				m_bestIntersection = intersection;
				m_selection = selection;
			}
		}
		bool isSelected() {
			return m_bestIntersection.valid();
		}
	};

	Face* m_face = 0;
	Plane3 m_plane;
	std::size_t m_width, m_height;
	TextureProjection m_projection;

	Matrix4 m_local2tex; //real projection
	Matrix4 m_tex2local; //real unprojection aka projection space basis aka texture axes
	Matrix4 m_faceLocal2tex; //x,y projected to the face for z = const
	Matrix4 m_faceTex2local;
	Vector3 m_origin;

	RenderablePivot m_pivot;
	Matrix4 m_pivot2world0; // original
	Matrix4 m_pivot2world; // transformed during transformation
	RenderablePoint m_pivotPoint;
	RenderableLines m_pivotLines;
	Matrix4 m_pivotLines2world;
	/* lines in uv space */
	RenderableLines m_Ulines;
	RenderableLines m_Vlines;
	Matrix4 m_lines2world; // line * ( transform during transformation ) * m_faceTex2local = world

	unsigned int m_gridU = 1; // n - 1 of U directed sub lines, 1-16
	unsigned int m_gridV = 1;
	RenderablePoint m_gridPointU; // control of U grid lines density, rendered on V axis
	RenderablePoint m_gridPointV;
	Vector2 m_gridSign; // orientation of controls relative to origin

	RenderableCircle m_circle;
	Matrix4 m_circle2world;

	Patch* m_patch = 0; //tracking face/patch mode by only nonzero pointer
	std::size_t m_patchWidth;
	std::size_t m_patchHeight;
	PatchControlArray m_patchCtrl;
	RenderablePoints m_patchRenderPoints;
	RenderableLines m_patchRenderLattice;
	RenderablePatchTexture m_patchRenderTex;
	const Shader* m_state_patch_raw = 0; // original patch texture shader
	Shader* m_state_patch = 0; // local patch texture overlay
	const char* m_state_patch_name = "$uvtool/patchtexture";

public:
	static Shader* m_state_line;
	static Shader* m_state_point;
	UVManipulator() : m_pivot( 32 ), m_circle( 8 << 3 ) {
		draw_circle( 8, 1, m_circle.m_vertices.data(), RemapXYZ() );
		m_circle.setColour( m_cGray );
		m_pivotPoint.setColour( m_cWhite );
		m_gridPointU.setColour( m_cWhite );
		m_gridPointV.setColour( m_cWhite );
		m_pivotLines.m_lines.resize( 4, PointVertex( vertex3f_identity, m_cWhite ) );
	}
	~UVManipulator() {
		patchShaderDestroy();
	}

private:
	void patchShaderConstruct(){
		patchShaderDestroy();

		OpenGLState state;
		GlobalOpenGLStateLibrary().getDefaultState( state );
		state.m_state = RENDER_FILL /*| RENDER_CULLFACE*/ | RENDER_TEXTURE | RENDER_COLOURWRITE | RENDER_LIGHTING | RENDER_SMOOTH;
		state.m_sort = OpenGLState::eSortOverlayLast;
		state.m_texture = m_patch->getShader()->getTexture().texture_number;

		GlobalOpenGLStateLibrary().insert( m_state_patch_name, state );
		m_state_patch = GlobalShaderCache().capture( m_state_patch_name );
	}

	void patchShaderDestroy(){
		if( m_state_patch ){
			m_state_patch = 0;
			GlobalShaderCache().release( m_state_patch_name );
			GlobalOpenGLStateLibrary().erase( m_state_patch_name );
		}
	}
	bool patchCtrl_isInside( std::size_t i ) const {
		return ( i % 2 || ( i / m_patchWidth ) % 2 );
	}
	template<typename Functor>
	void forEachEdge( const Functor& functor ) const {
		if( m_face ){
			const Winding& winding = m_face->getWinding();
			for( Winding::const_iterator next = winding.begin(), i = winding.end() - 1; next != winding.end(); i = next, ++next )
				functor( ( *i ).vertex, ( *next ).vertex );
		}
		else if( m_patch ){
			for( std::vector<PointVertex>::const_iterator i = m_patchRenderLattice.m_lines.begin(); i != m_patchRenderLattice.m_lines.end(); ++++i ){
				const Vector3 p0( matrix4_transformed_point( m_faceTex2local, ( *i ).vertex ) );
				const Vector3 p1( matrix4_transformed_point( m_faceTex2local, ( *( i + 1 ) ).vertex ) );
				if( vector3_length_squared( p1 - p0 ) > 0.1 )
					functor( p0, p1 );
			}
		}
	}
	template<typename Functor>
	void forEachPoint( const Functor& functor ) const {
		if( m_face ){
			const Winding& winding = m_face->getWinding();
			for( const auto& v : winding )
				functor( v.vertex );
		}
		else if( m_patch ){
			for( const auto& v : m_patchCtrl )
				functor( matrix4_transformed_point( m_faceTex2local, Vector3( v.m_texcoord ) ) );
		}
	}
	template<typename Functor>
	void forEachUVPoint( const Functor& functor ) const {
		if( m_face ){
			const Winding& winding = m_face->getWinding();
			for( const auto& v : winding )
				functor( matrix4_transformed_point( m_faceLocal2tex, v.vertex ) );
		}
		else if( m_patch ){
			for( const auto& v : m_patchCtrl )
				functor( Vector3( v.m_texcoord ) );
		}
	}
	bool projection_valid() const {
		return !( !std::isfinite( m_local2tex[0] ) //nan
		       || !std::isfinite( m_tex2local[0] ) //nan
		       || fabs( vector3_dot( m_plane.normal(), m_tex2local.z().vec3() ) ) < 1e-6 //projected along face
		       || vector3_length_squared( m_tex2local.x().vec3() ) < .01 //srsly scaled down, limit at max 10 textures per world unit
		       || vector3_length_squared( m_tex2local.y().vec3() ) < .01
		       || vector3_length_squared( m_tex2local.x().vec3() ) > 1e9 //very upscaled or product of nearly nan
		       || vector3_length_squared( m_tex2local.y().vec3() ) > 1e9 );
	}
	void UpdateFaceData( bool updateOrigin, bool updateLines = true ) {
		//!? todo fewer outer quads for large textures
		//!? todo auto subdivisions num, based on tex size and world scale
		//! todo update on undo/redo, when face stays the same, but transformed
		//! todo update on nudgeSelectedLeft and the rest, qe tool move w/o projection change or with tex lock off
		//+ todo put default origin to winding's UV aabb corner
		//+ todo disable 3d workzone in this manipulator mode
		if( m_face ){
			m_plane = m_face->getPlane().plane3();
			m_width = m_face->getShader().width();
			m_height = m_face->getShader().height();
//			m_face->GetTexdef( m_projection );
			m_projection = m_face->getTexdef().m_projection;

			Texdef_Construct_local2tex( m_projection, m_width, m_height, m_plane.normal(), m_local2tex );
			m_tex2local = matrix4_affine_inverse( m_local2tex );
		}
		else if( m_patch ){
			m_plane.normal() = m_patch->Calculate_AvgNormal();
			m_plane.dist() = vector3_dot( m_plane.normal(), m_patch->localAABB().origin );
			m_patchWidth = m_patch->getWidth();
			m_patchHeight = m_patch->getHeight();
			m_patchCtrl = m_patch->getControlPoints();
			m_state_patch_raw = m_patch->getShader();
			patchShaderConstruct();
			{	//! todo force or deduce orthogonal uv axes for convenience
				Vector3 wDir, hDir;
				m_patch->Calculate_AvgAxes( wDir, hDir );
				vector3_normalise( wDir );
				vector3_normalise( hDir );
//					globalOutputStream() << wDir << " wDir\n";
//					globalOutputStream() << hDir << " hDir\n";
//					globalOutputStream() << m_plane.normal() << " m_plane.normal()\n";

				/* find longest row and column */
				float wLength = 0, hLength = 0; //!? todo break, if some of these is 0
				std::size_t row = 0, col = 0;
				for ( std::size_t r = 0; r < m_patchHeight; ++r ){
					float length = 0;
					for ( std::size_t c = 0; c < m_patchWidth - 1; ++c ){
						length += vector3_length( m_patch->ctrlAt( r, c + 1 ).m_vertex - m_patch->ctrlAt( r, c ).m_vertex );
					}
					if( length - wLength > .1f || ( ( r == 0 || r == m_patchHeight - 1 ) && float_equal_epsilon( length, wLength, .1f ) ) ){ // prioritize first and last rows
						wLength = length;
						row = r;
					}
				}
				for ( std::size_t c = 0; c < m_patchWidth; ++c ){
					float length = 0;
					for ( std::size_t r = 0; r < m_patchHeight - 1; ++r ){
						length += vector3_length( m_patch->ctrlAt( r + 1, c ).m_vertex - m_patch->ctrlAt( r, c ).m_vertex );
					}
					if( length - hLength > .1f || ( ( c == 0 || c == m_patchWidth - 1 ) && float_equal_epsilon( length, hLength, .1f ) ) ){
						hLength = length;
						col = c;
					}
				}
				//! todo handle case, when uv start = end, like projection to cylinder
				//! todo consider max uv length to have manipulator size according to patch size
				/* pick 3 points at the found row and column */
				const PatchControl* p0, *p1, *p2;
				Vector3 v0, v1, v2;
				{
					float distW0 = 0, distW1 = 0;
					for ( std::size_t c = 0; c < col; ++c ){
						distW0 += vector3_length( m_patch->ctrlAt( row, c + 1 ).m_vertex - m_patch->ctrlAt( row, c ).m_vertex );
					}
					for ( std::size_t c = col; c < m_patchWidth - 1; ++c ){
						distW1 += vector3_length( m_patch->ctrlAt( row, c + 1 ).m_vertex - m_patch->ctrlAt( row, c ).m_vertex );
					}
					float distH0 = 0, distH1 = 0;
					for ( std::size_t r = 0; r < row; ++r ){
						distH0 += vector3_length( m_patch->ctrlAt( r + 1, col ).m_vertex - m_patch->ctrlAt( r, col ).m_vertex );
					}
					for ( std::size_t r = row; r < m_patchHeight - 1; ++r ){
						distH1 += vector3_length( m_patch->ctrlAt( r + 1, col ).m_vertex - m_patch->ctrlAt( r, col ).m_vertex );
					}

					if( ( distW0 > distH0 && distW0 > distH1 ) || ( distW1 > distH0 && distW1 > distH1 ) ){
						p0 = &m_patch->ctrlAt( 0, col );
						p1 = &m_patch->ctrlAt( m_patchHeight - 1, col );
						p2 = distW0 > distW1? &m_patch->ctrlAt( row, 0 ) : &m_patch->ctrlAt( row, m_patchWidth - 1 );
						v0 = m_patch->localAABB().origin
						     + hDir * vector3_dot( m_patch->localAABB().extents, Vector3( fabs( hDir.x() ), fabs( hDir.y() ), fabs( hDir.z() ) ) ) * 1.1
						     + wDir * ( distW0 - wLength / 2 );
						v1 = v0 + hDir * hLength;
						v2 = v0 + hDir * distH0 + ( distW0 > distW1? ( wDir * -distW0 ) : ( wDir * distW1 ) );
					}
					else{
						p0 = &m_patch->ctrlAt( row, 0 );
						p1 = &m_patch->ctrlAt( row, m_patchWidth - 1 );
						p2 = distH0 > distH1? &m_patch->ctrlAt( 0, col ) : &m_patch->ctrlAt( m_patchHeight - 1, col );
						v0 = m_patch->localAABB().origin
						     + wDir * vector3_dot( m_patch->localAABB().extents, Vector3( fabs( wDir.x() ), fabs( wDir.y() ), fabs( wDir.z() ) ) ) * 1.1
						     + hDir * ( distH0 - hLength / 2 );
						v1 = v0 + wDir * wLength;
						v2 = v0 + wDir * distW0 + ( distH0 > distH1? ( hDir * -distH0 ) : ( hDir * distH1 ) );
					}

					if( vector3_dot( plane3_for_points( v0, v1, v2 ).normal(), m_plane.normal() ) < 0 ){
						std::swap( p0, p1 );
						std::swap( v0, v1 );
					}
				}
				const DoubleVector3 vertices[3]{ v0, v1, v2 };
				const DoubleVector3 sts[3]{ DoubleVector3( p0->m_texcoord ),
				                            DoubleVector3( p1->m_texcoord ),
				                            DoubleVector3( p2->m_texcoord ) };
				Texdef_Construct_local2tex_from_ST( vertices, sts, m_local2tex );
				m_tex2local = matrix4_affine_inverse( m_local2tex );
			}
		}

//		globalOutputStream() << m_local2tex << " m_local2tex\n";
//		globalOutputStream() << m_tex2local << " m_tex2local\n";
		/* error checking */
		if( !projection_valid() ){
			m_selectedU = m_selectedV = 0;
			m_Ulines.m_lines.clear();
			m_Vlines.m_lines.clear();
			m_selectedPatchIndex = -1;
			return;
		}

		m_faceTex2local = m_tex2local;
		m_faceTex2local.x().vec3() = plane3_project_point( Plane3( m_plane.normal(), 0 ), m_tex2local.x().vec3(), m_tex2local.z().vec3() );
		m_faceTex2local.y().vec3() = plane3_project_point( Plane3( m_plane.normal(), 0 ), m_tex2local.y().vec3(), m_tex2local.z().vec3() );
		m_faceTex2local = matrix4_multiplied_by_matrix4( // adjust to have UV's z = 0: move the plane along m_tex2local.z() so that plane.dist() = 0
		                      matrix4_translation_for_vec3(
		                          m_tex2local.z().vec3() * ( m_plane.dist() - vector3_dot( m_plane.normal(), m_tex2local.t().vec3() ) )
		                          / vector3_dot( m_plane.normal(), m_tex2local.z().vec3() )
		                      ),
		                      m_faceTex2local );
		m_faceLocal2tex = matrix4_affine_inverse( m_faceTex2local );

		if( m_patch ){
			m_patchRenderPoints.m_points.clear();
			m_patchRenderPoints.m_points.reserve( m_patchWidth * m_patchHeight );
			for( std::size_t i = 0; i < m_patchCtrl.size(); ++i ){
				m_patchRenderPoints.m_points.emplace_back( vertex3f_for_vector3( Vector3( m_patchCtrl[i].m_texcoord ) ), patchCtrl_isInside( i )? m_cPin : m_cGree );
			}

			m_patchRenderLattice.m_lines.clear();
			m_patchRenderLattice.m_lines.reserve( ( ( m_patchWidth - 1 ) * m_patchHeight + ( m_patchHeight - 1 ) * m_patchWidth ) * 2 );
			for ( std::size_t r = 0; r < m_patchHeight; ++r ){
				for ( std::size_t c = 0; c < m_patchWidth - 1; ++c ){
					const Vector2& a = m_patch->ctrlAt( r, c ).m_texcoord;
					const Vector2& b = m_patch->ctrlAt( r, c + 1 ).m_texcoord;
					m_patchRenderLattice.m_lines.emplace_back( vertex3f_for_vector3( Vector3( a ) ), m_cOrang );
					m_patchRenderLattice.m_lines.emplace_back( vertex3f_for_vector3( Vector3( b ) ), m_cOrang );
				}
			}
			for ( std::size_t c = 0; c < m_patchWidth; ++c ){
				for ( std::size_t r = 0; r < m_patchHeight - 1; ++r ){
					const Vector2& a = m_patch->ctrlAt( r, c ).m_texcoord;
					const Vector2& b = m_patch->ctrlAt( r + 1, c ).m_texcoord;
					m_patchRenderLattice.m_lines.emplace_back( vertex3f_for_vector3( Vector3( a ) ), m_cOrang );
					m_patchRenderLattice.m_lines.emplace_back( vertex3f_for_vector3( Vector3( b ) ), m_cOrang );
				}
			}

			m_patchRenderTex.m_trianglesIndices.clear();
			m_patchRenderTex.m_trianglesIndices.reserve( ( m_patchHeight - 1 ) * ( m_patchWidth - 1 ) * 2 * 3 );
			const PatchControlArray& pc = m_patch->getControlPointsTransformed();
			m_patchRenderTex.m_patchControlArray = &pc;
			const double degenerate_epsilon = 1e-5;
			for ( std::size_t r = 0; r < m_patchHeight - 1; ++r ){
				for ( std::size_t c = 0; c < m_patchWidth - 1; ++c ){
					const RenderIndex i0 = m_patchWidth * r + c;
					const RenderIndex i1 = m_patchWidth * ( r + 1 ) + c;
					const RenderIndex i2 = m_patchWidth * ( r + 1 ) + c + 1;
					const RenderIndex i3 = m_patchWidth * r + c + 1;
					double cross = vector2_cross( pc[i2].m_texcoord - pc[i0].m_texcoord, pc[i1].m_texcoord - pc[i0].m_texcoord );
					if( !float_equal_epsilon( cross, 0, degenerate_epsilon ) ){
						m_patchRenderTex.m_trianglesIndices.push_back( i0 );
						m_patchRenderTex.m_trianglesIndices.push_back( i1 );
						m_patchRenderTex.m_trianglesIndices.push_back( i2 );
						if( cross < 0 )
							std::swap( *( m_patchRenderTex.m_trianglesIndices.end() - 1 ), *( m_patchRenderTex.m_trianglesIndices.end() - 2 ) );
					}
					cross = vector2_cross( pc[i3].m_texcoord - pc[i0].m_texcoord, pc[i2].m_texcoord - pc[i0].m_texcoord );
					if( !float_equal_epsilon( cross, 0, degenerate_epsilon ) ){
						m_patchRenderTex.m_trianglesIndices.push_back( i0 );
						m_patchRenderTex.m_trianglesIndices.push_back( i2 );
						m_patchRenderTex.m_trianglesIndices.push_back( i3 );
						if( cross < 0 )
							std::swap( *( m_patchRenderTex.m_trianglesIndices.end() - 1 ), *( m_patchRenderTex.m_trianglesIndices.end() - 2 ) );
					}
				}
			}
			if( m_patchRenderTex.m_trianglesIndices.size() == 0 ){ // try to make at least one triangle or more
				RenderIndex i0 = 0, i1 = 1, i2;
				for( ; i1 < pc.size(); ++i1 ){
					if( vector2_length( pc[i1].m_texcoord - pc[i0].m_texcoord ) > degenerate_epsilon ){
						i2 = i1 + 1;
						for( ; i2 < pc.size(); ++i2 ){
							const double cross = vector2_cross( pc[i2].m_texcoord - pc[i0].m_texcoord, pc[i1].m_texcoord - pc[i0].m_texcoord );
							if( !float_equal_epsilon( cross, 0, degenerate_epsilon ) ){
								m_patchRenderTex.m_trianglesIndices.push_back( i0 );
								m_patchRenderTex.m_trianglesIndices.push_back( i1 );
								m_patchRenderTex.m_trianglesIndices.push_back( i2 );
								if( cross < 0 )
									std::swap( *( m_patchRenderTex.m_trianglesIndices.end() - 1 ), *( m_patchRenderTex.m_trianglesIndices.end() - 2 ) );
								break;
							}
						}
					}
				}
			}
		}

		Vector2 min( FLT_MAX, FLT_MAX );
		Vector2 max( -FLT_MAX, -FLT_MAX );
		forEachUVPoint( [&]( const Vector3& point ){
			min.x() = std::min( min.x(), point.x() );
			max.x() = std::max( max.x(), point.x() );
			min.y() = std::min( min.y(), point.y() );
			max.y() = std::max( max.y(), point.y() );
		} );

		if( updateOrigin )
			m_origin = matrix4_transformed_point( m_faceTex2local, Vector3( min ) );

		const Vector3 uv_origin = matrix4_transformed_point( m_faceLocal2tex, m_origin );

		{	// grid grain controls, on the polygon side of origin
			m_gridSign.x() = max.y() - uv_origin.y() >=  uv_origin.y() - min.y()? 1 : -1;
			m_gridSign.y() = max.x() - uv_origin.x() >=  uv_origin.x() - min.x()? 1 : -1;
			m_gridPointU.m_point.vertex = Vertex3f( uv_origin.x(),
			                                        float_to_integer( uv_origin.y() + m_gridSign.x() * .25 ) + m_gridSign.x() * ( 1 - 1.0 / std::max( float( m_gridU ), 1.8f ) ),
			                                        0 );
			m_gridPointV.m_point.vertex = Vertex3f( float_to_integer( uv_origin.x() + m_gridSign.y() * .25 ) + m_gridSign.y() * ( 1 - 1.0 / std::max( float( m_gridV ), 1.8f ) ),
			                                        uv_origin.y(),
			                                        0 );
		}

		m_pivot2world = m_tex2local;
		vector3_normalise( m_pivot2world.x().vec3() );
		vector3_normalise( m_pivot2world.y().vec3() );
		m_pivot2world.t().vec3() = m_origin;
		m_pivot2world0 = m_pivot2world;

		{
			float bestDist = 0;
			forEachPoint( [&]( const Vector3& point ){
				const float dist = vector3_length_squared( point - m_origin );
				if( dist > bestDist ){
					bestDist = dist;
				}
			} );
			bestDist = sqrt( bestDist );
			m_circle2world = g_matrix4_identity;
			ComputeAxisBase( m_plane.normal(), m_circle2world.x().vec3(), m_circle2world.y().vec3() );
			m_circle2world.x().vec3() *= bestDist;
			m_circle2world.y().vec3() *= bestDist;
			m_circle2world.z().vec3() = m_plane.normal();
			m_circle2world.t().vec3() = m_origin;
		}

		min -= Vector2( 5, 5 );
		max += Vector2( 5, 5 );
		min.x() = float_to_integer( min.x() );
		min.y() = float_to_integer( min.y() );
		max.x() = float_to_integer( max.x() );
		max.y() = float_to_integer( max.y() );

		m_selectedU = m_selectedV = 0;
		m_selectedPatchIndex = -1;
		m_lines2world = m_faceTex2local;
		m_pivotLines2world = m_faceTex2local;
		if( updateLines ){
			const int imax = float_to_integer( max.y() - min.y() ) + 1;
			m_Ulines.m_lines.clear();
			m_Ulines.m_lines.reserve( ( imax + ( m_gridU - 1 ) * ( imax - 1 ) ) * 2 );
			for( int i = 0; i < imax; ++i ){
				if( i != 0 ){
					for( std::size_t j = m_gridU - 1; j != 0; --j ){ //subgrid lines
						m_Ulines.m_lines.emplace_back( Vertex3f( min.x(), min.y() + i - static_cast<float>( j ) / m_gridU, 0 ), m_cGrayer );
						m_Ulines.m_lines.emplace_back( Vertex3f( max.x(), min.y() + i - static_cast<float>( j ) / m_gridU, 0 ), m_cGrayer );
					}
				}
				m_Ulines.m_lines.emplace_back( Vertex3f( min.x(), min.y() + i, 0 ), m_cGray );
				m_Ulines.m_lines.emplace_back( Vertex3f( max.x(), min.y() + i, 0 ), m_cGray );
			}
		}
		if( updateLines ){
			const int imax = float_to_integer( max.x() - min.x() ) + 1;
			m_Vlines.m_lines.clear();
			m_Vlines.m_lines.reserve( ( imax + ( m_gridV - 1 ) * ( imax - 1 ) ) * 2 );
			for( int i = 0; i < imax; ++i ){
				if( i != 0 ){
					for( std::size_t j = m_gridV - 1; j != 0; --j ){
						m_Vlines.m_lines.emplace_back( Vertex3f( min.x() + i - static_cast<float>( j ) / m_gridV, min.y(), 0 ), m_cGrayer );
						m_Vlines.m_lines.emplace_back( Vertex3f( min.x() + i - static_cast<float>( j ) / m_gridV, max.y(), 0 ), m_cGrayer );
					}
				}
				m_Vlines.m_lines.emplace_back( Vertex3f( min.x() + i, min.y(), 0 ), m_cGray );
				m_Vlines.m_lines.emplace_back( Vertex3f( min.x() + i, max.y(), 0 ), m_cGray );
			}
		}
		{
			{	// u pivot line
				m_pivotLines.m_lines[0].vertex = Vertex3f( min.x(), uv_origin.y(), 0 );
				m_pivotLines.m_lines[1].vertex = Vertex3f( max.x(), uv_origin.y(), 0 );
			}
			{	// v pivot line
				m_pivotLines.m_lines[2].vertex = Vertex3f( uv_origin.x(), min.y(), 0 );
				m_pivotLines.m_lines[3].vertex = Vertex3f( uv_origin.x(), max.y(), 0 );
			}
		}
	}
	bool UpdateData() {
		if( !g_SelectedFaceInstances.empty() ){
			Face* face = &g_SelectedFaceInstances.last().getFace();
			if( m_face != face ){
				m_face = face;
				m_patch = 0;
				UpdateFaceData( true );
			}
			else if( memcmp( &m_projection, &m_face->getTexdef().m_projection, sizeof( TextureProjection ) ) != 0
			         || m_width != m_face->getShader().width()
			         || m_height != m_face->getShader().height() ) {
				UpdateFaceData( !projection_valid() ); // updateOrigin when prev state was invalid on the same face
			}
			return projection_valid();
		}
		else if( GlobalSelectionSystem().countSelected() != 0 ){
			Patch* patch = Node_getPatch( GlobalSelectionSystem().ultimateSelected().path().top() );
			if( patch ){
				if( m_patch != patch ){
					m_patch = patch;
					m_face = 0;
					UpdateFaceData( true );
				}
				else if( m_patchWidth != m_patch->getWidth()
				      || m_patchHeight != m_patch->getHeight()
				      || memcmp( m_patchCtrl.data(), m_patch->getControlPoints().data(), sizeof( *m_patchCtrl.data() ) * m_patchCtrl.size() ) != 0
				      || m_state_patch_raw != m_patch->getShader() ){
					UpdateFaceData( !projection_valid() ); // updateOrigin when prev state was invalid on the same patch
				}
				return projection_valid();
			}
		}
		return false;

	}
public:
	void render( Renderer& renderer, const VolumeTest& volume, const Matrix4& pivot2world ) override {
		if( volume.fill() && UpdateData() ){
			if( m_patch ){
				renderer.SetState( const_cast<Shader*>( m_state_patch ), Renderer::eFullMaterials );
				renderer.addRenderable( m_patchRenderTex, m_lines2world );
			}
			renderer.SetState( m_state_line, Renderer::eFullMaterials );
			renderer.addRenderable( m_Ulines, m_lines2world );
			renderer.addRenderable( m_Vlines, m_lines2world );
			renderer.addRenderable( m_pivotLines, m_pivotLines2world );
			if( m_patch )
				renderer.addRenderable( m_patchRenderLattice, m_faceTex2local );

			//fix pivot position for better visibility
			m_pivot.render( renderer, volume, matrix4_multiplied_by_matrix4( matrix4_translation_for_vec3( vector3_normalised( volume.getViewer() - m_origin ) ), m_pivot2world ) );

			renderer.addRenderable( m_circle, m_circle2world );

			renderer.SetState( m_state_point, Renderer::eFullMaterials );
			if( m_patch )
				renderer.addRenderable( m_patchRenderPoints, m_faceTex2local );
			renderer.addRenderable( m_pivotPoint, m_pivot2world );
			renderer.addRenderable( m_gridPointU, m_pivotLines2world );
			renderer.addRenderable( m_gridPointV, m_pivotLines2world );
		}
	}
	void testSelect( const View& view, const Matrix4& pivot2world ) override {
		//!? todo fix: eUV selection possibility may be blocked by the circle
		if( !view.fill() || !UpdateData() ){
			m_isSelected = false;
			return;
		}

		UVSelector selector;

		if( g_modifiers == c_modifierAlt ) // only try skew with alt // note also grabs eTex
			goto testSelectUVlines;
		if( g_modifiers != c_modifierNone )
			return applySelection( selector.m_selection, nullptr, nullptr, selector.m_index );

		{	// try pivot point
			const Matrix4 local2view( matrix4_multiplied_by_matrix4( view.GetViewMatrix(), m_pivot2world ) );
			SelectionIntersection best;
			Point_BestPoint( local2view, m_pivotPoint.m_point.vertex, best );
			selector.addIntersection( best, ePivot );
		}

		if( !selector.isSelected() ){ // try grid control points
			const Matrix4 local2view( matrix4_multiplied_by_matrix4( view.GetViewMatrix(), m_faceTex2local ) );
			SelectionIntersection best;
			Point_BestPoint( local2view, m_gridPointU.m_point.vertex, best );
			selector.addIntersection( best, eGridU );
			Point_BestPoint( local2view, m_gridPointV.m_point.vertex, best );
			selector.addIntersection( best, eGridV );
		}

		if( !selector.isSelected() && m_patch ){ // try patch points
			const Matrix4 local2view( matrix4_multiplied_by_matrix4( view.GetViewMatrix(), m_faceTex2local ) );
			SelectionIntersection best;
			for( std::size_t i = 0; i < m_patchRenderPoints.m_points.size(); ++i ){
				Point_BestPoint( local2view, m_patchRenderPoints.m_points[i], best );
				selector.addIntersection( best, ePatchPoint, i );
			}
		}
		if( !selector.isSelected() && m_patch ){ // try patch rows, columns
			const Matrix4 local2view( matrix4_multiplied_by_matrix4( view.GetViewMatrix(), m_faceTex2local ) );
			SelectionIntersection best;
			for ( std::size_t r = 0; r < m_patchHeight; ++r ){
				for ( std::size_t c = 0; c < m_patchWidth - 1; ++c ){
					Line_BestPoint( local2view, &m_patchRenderLattice.m_lines[( r * ( m_patchWidth - 1 ) + c ) * 2], best );
					selector.addIntersection( best, ePatchRow, r );
				}
			}
			for ( std::size_t c = 0; c < m_patchWidth; ++c ){
				for ( std::size_t r = 0; r < m_patchHeight - 1; ++r ){
					Line_BestPoint( local2view, &m_patchRenderLattice.m_lines[( m_patchWidth - 1 ) * m_patchHeight * 2 + ( c * ( m_patchHeight - 1 ) + r ) * 2], best );
					selector.addIntersection( best, ePatchColumn, c );
				}
			}
		}

		if( !selector.isSelected() ){ // try circle
			const Matrix4 local2view( matrix4_multiplied_by_matrix4( view.GetViewMatrix(), m_circle2world ) );
			SelectionIntersection best;
			LineLoop_BestPoint( local2view, m_circle.m_vertices.data(), m_circle.m_vertices.size(), best );
			selector.addIntersection( best, eCircle );
		}

		if( !selector.isSelected() ){ // try pivot lines
			const Matrix4 local2view( matrix4_multiplied_by_matrix4( view.GetViewMatrix(), m_faceTex2local ) );
			SelectionIntersection best;
			Line_BestPoint( local2view, &m_pivotLines.m_lines[0], best );
			selector.addIntersection( best, ePivotU );
			Line_BestPoint( local2view, &m_pivotLines.m_lines[2], best );
			selector.addIntersection( best, ePivotV );
		}
testSelectUVlines:
		PointVertex* selectedU = 0;
		PointVertex* selectedV = 0;
		EUVSelection& selection = selector.m_selection;
		if( !selector.isSelected() ){ // try UV lines
/*
            -|------
             |
             |
V line center| - -  tex U center - -
             |         tex
             |          V
      -cross-|-----U line center-----|
             |
*/
			// special fuckage with the grid for better distinguishing of user's intentions
			// better picking of tex, only line for skew or scale with dense grid
			const Matrix4 screen2world( matrix4_full_inverse( view.GetViewMatrix() ) );
			const DoubleRay ray = ray_for_points( vector4_projected( matrix4_transformed_vector4( screen2world, BasicVector4<double>( 0, 0, -1, 1 ) ) ),
			                                      vector4_projected( matrix4_transformed_vector4( screen2world, BasicVector4<double>( 0, 0, 1, 1 ) ) ) );
			const DoubleVector3 hit = ray_intersect_plane( ray, m_plane );
			const Vector3 uvhit = matrix4_transformed_point( m_faceLocal2tex, hit );
			if( fabs( vector3_dot( ray.direction, m_plane.normal() ) ) > 1e-6
			 && !m_Ulines.m_lines.empty()
			 && !m_Vlines.m_lines.empty()
			 && matrix4_transformed_vector4( view.GetViewMatrix(), Vector4( hit, 1 ) ).w() > 0 ){
				PointVertex* closestU = &m_Ulines.m_lines[std::min( m_Ulines.m_lines.size() - 2,
				                                  static_cast<std::size_t>( float_to_integer( std::max( 0.f, uvhit.y() - m_Ulines.m_lines.front().vertex.y() ) * m_gridU ) * 2 ) )];
				PointVertex* closestV = &m_Vlines.m_lines[std::min( m_Vlines.m_lines.size() - 2,
				                                  static_cast<std::size_t>( float_to_integer( std::max( 0.f, uvhit.x() - m_Vlines.m_lines.front().vertex.x() ) * m_gridV ) * 2 ) )];
				const Vector2 sign( uvhit.y() > closestU->vertex.y()? 1 : -1, uvhit.x() > closestV->vertex.x()? 1 : -1 ); //hit in positive or negative part of lines u, v
				const PointVertex pCross( Vertex3f( closestV->vertex.x(), closestU->vertex.y(), 0 ) );
				const PointVertex pUcenter( Vertex3f( closestV->vertex.x() + sign.y() / ( m_gridV * 2 ), closestU->vertex.y(), 0 ) );
				const PointVertex pVcenter( Vertex3f( closestV->vertex.x(), closestU->vertex.y() + sign.x() / ( m_gridU * 2 ), 0 ) );

				PointVertex pTexUcenter[2]{ *closestU, *( closestU + 1 ) };
				pTexUcenter[0].vertex.y() = pTexUcenter[1].vertex.y() = pVcenter.vertex.y();
				PointVertex pTexVcenter[2]{ *closestV, *( closestV + 1 ) };
				pTexVcenter[0].vertex.x() = pTexVcenter[1].vertex.x() = pUcenter.vertex.x();

				SelectionIntersection iCross, iUcenter, iVcenter, iTexUcenter, iTexVcenter, iU, iV, iNull;

				const Matrix4 local2view( matrix4_multiplied_by_matrix4( view.GetViewMatrix(), m_faceTex2local ) );
#if defined( DEBUG_SELECTION )
				g_render_clipped.construct( view.GetViewMatrix() );
#endif
				Line_BestPoint( local2view, closestU, iU );
				Line_BestPoint( local2view, closestV, iV );
				Line_BestPoint( local2view, pTexUcenter, iTexUcenter );
				Line_BestPoint( local2view, pTexVcenter, iTexVcenter );
				const bool uselected = iU < iNull;
				const bool vselected = iV < iNull;
				if( !uselected && !vselected ){ //no lines hit, definitely tex
					selection = eTex;
				}
				else if( ( !uselected || iTexUcenter < iU ) && ( !vselected || iTexVcenter < iV ) ){ //yes lines, but tex ones are closer
					selection = eTex;
				}
				else if( uselected != vselected ){ //only line selected
					if( uselected ){
						selection = g_modifiers == c_modifierAlt? eSkewU : eU;
						selectedU = closestU;
					}
					else{
						selection = g_modifiers == c_modifierAlt? eSkewV : eV;
						selectedV = closestV;
					}
				}
				else{ //two lines hit
					if( g_modifiers == c_modifierAlt ){ //pick only line for skew
						if( iU < iV ){
							selection = eSkewU;
							selectedU = closestU;
						}
						else{
							selection = eSkewV;
							selectedV = closestV;
						}
					}
					else{
						Point_BestPoint( local2view, pUcenter, iUcenter );
						Point_BestPoint( local2view, pVcenter, iVcenter );
						Point_BestPoint( local2view, pCross, iCross );
						const bool ucenter = iUcenter < iNull;
						const bool vcenter = iVcenter < iNull;
						if( !ucenter && !vcenter ){ // no centers, definitely two lines
							selection = eUV;
							selectedU = closestU;
							selectedV = closestV;
						}
						else if( iCross < iUcenter && iCross < iVcenter ){ // some center(s), cross is closer = two lines
							selection = eUV;
							selectedU = closestU;
							selectedV = closestV;
						}
						else{ // some center(s), pick closest line
							if( iUcenter < iVcenter ){
								selection = eU;
								selectedU = closestU;
							}
							else{
								selection = eV;
								selectedV = closestV;
							}
						}
					}
				}
			}
		}

		applySelection( selector.m_selection, selectedU, selectedV, selector.m_index );
	}
private:
	void applySelection( EUVSelection selection, PointVertex* selectedU, PointVertex* selectedV, int selectedPatchIndex ){
		if( m_selection != selection
		 || m_selectedU != selectedU
		 || m_selectedV != selectedV
		 || m_selectedPatchIndex != selectedPatchIndex ){
			if( m_selection != selection ){
				switch ( m_selection )
				{
				case ePivot:
					m_pivotPoint.m_point.colour = m_cWhite;
					break;
				case eGridU:
					m_gridPointU.m_point.colour = m_cWhite;
					break;
				case eGridV:
					m_gridPointV.m_point.colour = m_cWhite;
					break;
				case eCircle:
					m_circle.setColour( m_cGray );
					break;
				case ePivotU:
					m_pivotLines.m_lines[0].colour = m_cWhite;
					m_pivotLines.m_lines[1].colour = m_cWhite;
					break;
				case ePivotV:
					m_pivotLines.m_lines[2].colour = m_cWhite;
					m_pivotLines.m_lines[3].colour = m_cWhite;
					break;
				default:
					break;
				}
				switch ( selection )
				{
				case ePivot:
					m_pivotPoint.m_point.colour = m_cRed;
					break;
				case eGridU:
					m_gridPointU.m_point.colour = m_cRed;
					break;
				case eGridV:
					m_gridPointV.m_point.colour = m_cRed;
					break;
				case eCircle:
					m_circle.setColour( g_colour_selected );
					break;
				case ePivotU:
					m_pivotLines.m_lines[0].colour = m_cRed;
					m_pivotLines.m_lines[1].colour = m_cRed;
					break;
				case ePivotV:
					m_pivotLines.m_lines[2].colour = m_cRed;
					m_pivotLines.m_lines[3].colour = m_cRed;
					break;
				default:
					break;
				}
			}

			const Colour4b colour_selected = g_modifiers == c_modifierAlt? m_cGreen : g_colour_selected;
			if( m_selectedU != selectedU || m_selection != selection ){ // selected line changed or not, but scale<->skew modes exchanged
				if( m_selectedU )
					m_selectedU->colour =
					( m_selectedU + 1 )->colour = ( ( m_selectedU - &m_Ulines.m_lines[0] ) / 2 ) % m_gridU == 0? m_cGray : m_cGrayer;
				if( selectedU )
					selectedU->colour =
					( selectedU + 1 )->colour = colour_selected;
			}
			if( m_selectedV != selectedV || m_selection != selection ){
				if( m_selectedV )
					m_selectedV->colour =
					( m_selectedV + 1 )->colour = ( ( m_selectedV - &m_Vlines.m_lines[0] ) / 2 ) % m_gridV == 0? m_cGray : m_cGrayer;
				if( selectedV )
					selectedV->colour =
					( selectedV + 1 )->colour = colour_selected;
			}

			if( m_selectedPatchIndex != selectedPatchIndex || m_selection != selection ){
				if( m_selectedPatchIndex >= 0 ){
					switch ( m_selection )
					{
					case ePatchPoint:
						m_patchRenderPoints.m_points[m_selectedPatchIndex].colour = patchCtrl_isInside( m_selectedPatchIndex )? m_cPin : m_cGree;
						break;
					case ePatchRow:
						for ( std::size_t c = 0; c < m_patchWidth - 1; ++c ){
							const std::size_t i = ( m_selectedPatchIndex * ( m_patchWidth - 1 ) + c ) * 2;
							m_patchRenderLattice.m_lines[i].colour =
							m_patchRenderLattice.m_lines[i + 1].colour = m_cOrang;
						}
						for ( std::size_t c = 0; c < m_patchWidth; ++c ){
							const std::size_t i = m_selectedPatchIndex * m_patchWidth + c;
							m_patchRenderPoints.m_points[i].colour = patchCtrl_isInside( i )? m_cPin : m_cGree;
						}
						break;
					case ePatchColumn:
						for ( std::size_t r = 0; r < m_patchHeight - 1; ++r ){
							const std::size_t i = ( m_patchWidth - 1 ) * m_patchHeight * 2 + ( m_selectedPatchIndex * ( m_patchHeight - 1 ) + r ) * 2;
							m_patchRenderLattice.m_lines[i].colour =
							m_patchRenderLattice.m_lines[i + 1].colour = m_cOrang;
						}
						for ( std::size_t r = 0; r < m_patchHeight; ++r ){
							const std::size_t i = r * m_patchWidth + m_selectedPatchIndex;
							m_patchRenderPoints.m_points[i].colour = patchCtrl_isInside( i )? m_cPin : m_cGree;
						}
						break;
					default:
						break;
					}
				}
				if( selectedPatchIndex >= 0 ){
					switch ( selection )
					{
					case ePatchPoint:
						m_patchRenderPoints.m_points[selectedPatchIndex].colour = patchCtrl_isInside( selectedPatchIndex )? m_cPink : m_cGreen;
						break;
					case ePatchRow:
						for ( std::size_t c = 0; c < m_patchWidth - 1; ++c ){
							const std::size_t i = ( selectedPatchIndex * ( m_patchWidth - 1 ) + c ) * 2;
							m_patchRenderLattice.m_lines[i].colour =
							m_patchRenderLattice.m_lines[i + 1].colour = m_cOrange;
						}
						for ( std::size_t c = 0; c < m_patchWidth; ++c ){
							const std::size_t i = selectedPatchIndex * m_patchWidth + c;
							m_patchRenderPoints.m_points[i].colour = patchCtrl_isInside( i )? m_cPink : m_cGreen;
						}
						break;
					case ePatchColumn:
						for ( std::size_t r = 0; r < m_patchHeight - 1; ++r ){
							const std::size_t i = ( m_patchWidth - 1 ) * m_patchHeight * 2 + ( selectedPatchIndex * ( m_patchHeight - 1 ) + r ) * 2;
							m_patchRenderLattice.m_lines[i].colour =
							m_patchRenderLattice.m_lines[i + 1].colour = m_cOrange;
						}
						for ( std::size_t r = 0; r < m_patchHeight; ++r ){
							const std::size_t i = r * m_patchWidth + selectedPatchIndex;
							m_patchRenderPoints.m_points[i].colour = patchCtrl_isInside( i )? m_cPink : m_cGreen;
						}
						break;
					default:
						break;
					}
				}
			}

			m_selection = selection;
			m_selectedU = selectedU;
			m_selectedV = selectedV;
			m_selectedPatchIndex = selectedPatchIndex;
			SceneChangeNotify();
		}
		m_isSelected = ( selection != eNone );
	}
	void commitTransform( const Matrix4& transform ) const {
		if( m_face ){
			m_face->transform_texdef( transform, m_origin ); //! todo make SI update after Brush_textureChanged(); same problem after brush moved with tex lock
		} // also after Patch_textureChanged(); calling them now in this->freezeTransform() works good nuff
		else if( m_patch ){
			const Matrix4 uvTransform = transform_local2object( matrix4_affine_inverse( transform ), m_faceLocal2tex, m_faceTex2local );
			for( std::size_t i = 0; i < m_patchCtrl.size(); ++i ){
				const Vector3 uv = matrix4_transformed_point( uvTransform, Vector3( m_patchCtrl[i].m_texcoord ) );
				m_patch->getControlPointsTransformed()[i].m_texcoord = uv.vec2();
			}
//			m_patch->controlPointsChanged();
			m_patch->UpdateCachedData();
		}
		SceneChangeNotify();
	}
	/* Manipulatable */
	Vector3 m_start;
public:
	void Construct( const Matrix4& device2manip, const DeviceVector device_point, const AABB& bounds, const Vector3& transform_origin ) override {
		m_start = point_on_plane( m_plane, m_view->GetViewMatrix(), device_point );
	}
	//!? fix meaningless undo on grid/origin change, then click tex or lines
	//!? todo no snap mode with alt modifier
	void Transform( const Matrix4& manip2object, const Matrix4& device2manip, const DeviceVector device_point ) override {
		const Vector3 current = point_on_plane( m_plane, m_view->GetViewMatrix(), device_point );

		const bool snap = g_modifiers.shift(), snapHard = g_modifiers.ctrl();

		const class Snapper
		{
			float m_x; //uv axis to screen coef
			float m_y;
		public:
			Snapper( const Vector3& current, const Matrix4& faceTex2local ) {
				Vector3 scale( m_view->GetViewport().x().x(), m_view->GetViewport().y().y(), 0 );
				scale /= float{ std::max( scale.x(), scale.y() ) }; // normalise to be consistent over screen width & height
				const Matrix4 proj = matrix4_multiplied_by_matrix4( matrix4_scale_for_vec3( scale ), m_view->GetViewMatrix() );
				// get unary world displacements over uv axes to screenspace
				const Vector3 curr = vector4_projected( matrix4_transformed_vector4( proj, Vector4( current, 1 ) ) );
				const Vector3 x = vector4_projected( matrix4_transformed_vector4( proj, Vector4( current + vector3_normalised( faceTex2local.x().vec3() ), 1 ) ) );
				const Vector3 y = vector4_projected( matrix4_transformed_vector4( proj, Vector4( current + vector3_normalised( faceTex2local.y().vec3() ), 1 ) ) );
				m_x = vector3_length( x - curr ) * vector3_length( faceTex2local.x().vec3() ); // consider uv space scaling
				m_y = vector3_length( y - curr ) * vector3_length( faceTex2local.y().vec3() );
			}
			bool x_snaps( float uv_dist, float epsilon = .01f ) const {
				return uv_dist * m_x < epsilon;
			}
			bool y_snaps( float uv_dist, float epsilon = .01f ) const {
				return uv_dist * m_y < epsilon;
			}
		} snapper( current, m_faceTex2local );

		switch ( m_selection )
		{
		case ePivot:
			{
				const Vector3 uv_origin_start = matrix4_transformed_point( m_faceLocal2tex, m_origin );
				const Vector3 uv_origin = matrix4_transformed_point( m_faceLocal2tex, current );
				float bestDistU = FLT_MAX;
				float bestDistV = FLT_MAX;
				float snapToU = 0;
				float snapToV = 0;
				for( std::vector<PointVertex>::const_iterator i = m_Ulines.m_lines.begin(); i != m_Ulines.m_lines.end(); ++++i ){
					const float dist = fabs( ( *i ).vertex.y() - uv_origin.y() );
					if( dist < bestDistU ){
						bestDistU = dist;
						snapToU = ( *i ).vertex.y();
					}
				}
				for( std::vector<PointVertex>::const_iterator i = m_Vlines.m_lines.begin(); i != m_Vlines.m_lines.end(); ++++i ){
					const float dist = fabs( ( *i ).vertex.x() - uv_origin.x() );
					if( dist < bestDistV ){
						bestDistV = dist;
						snapToV = ( *i ).vertex.x();
					}
				}
				forEachUVPoint( [&]( const Vector3& point ){
					const float distU = fabs( point.y() - uv_origin.y() );
					if( distU < bestDistU ){
						bestDistU = distU;
						snapToU = point.y();
					}
					const float distV = fabs( point.x() - uv_origin.x() );
					if( distV < bestDistV ){
						bestDistV = distV;
						snapToV = point.x();
					}
				} );
				Vector3 result( uv_origin_start );
				if( snapper.y_snaps( bestDistU ) || snapHard ){
					result.y() = snapToU;
				}
				else{
					result.y() = uv_origin.y();
				}
				if( snapper.x_snaps( bestDistV ) || snapHard ){
					result.x() = snapToV;
				}
				else{
					result.x() = uv_origin.x();
				}
				m_origin = matrix4_transformed_point( m_faceTex2local, result );
				UpdateFaceData( false, false );
				SceneChangeNotify();
			}
			break;
		case ePivotU:
			{
				const Vector3 uv_origin_start = matrix4_transformed_point( m_faceLocal2tex, m_origin );
				const Vector3 uv_origin = matrix4_transformed_point( m_faceLocal2tex, current );
				float bestDist = FLT_MAX;
				float snapTo = 0;
				for( std::vector<PointVertex>::const_iterator i = m_Ulines.m_lines.begin(); i != m_Ulines.m_lines.end(); ++++i ){
					const float dist = fabs( ( *i ).vertex.y() - uv_origin.y() );
					if( dist < bestDist ){
						bestDist = dist;
						snapTo = ( *i ).vertex.y();
					}
				}
				forEachUVPoint( [&]( const Vector3& point ){
					const float dist = fabs( point.y() - uv_origin.y() );
					if( dist < bestDist ){
						bestDist = dist;
						snapTo = point.y();
					}
				} );
				Vector3 result( uv_origin_start );
				if( snapper.y_snaps( bestDist ) || snapHard ){
					result.y() = snapTo;
				}
				else{
					result.y() = uv_origin.y();
				}
				m_origin = matrix4_transformed_point( m_faceTex2local, result );
				UpdateFaceData( false, false );
				SceneChangeNotify();
			}
			break;
		case ePivotV:
			{
				const Vector3 uv_origin_start = matrix4_transformed_point( m_faceLocal2tex, m_origin );
				const Vector3 uv_origin = matrix4_transformed_point( m_faceLocal2tex, current );
				float bestDist = FLT_MAX;
				float snapTo = 0;
				for( std::vector<PointVertex>::const_iterator i = m_Vlines.m_lines.begin(); i != m_Vlines.m_lines.end(); ++++i ){
					const float dist = fabs( ( *i ).vertex.x() - uv_origin.x() );
					if( dist < bestDist ){
						bestDist = dist;
						snapTo = ( *i ).vertex.x();
					}
				}
				forEachUVPoint( [&]( const Vector3& point ){
					const float dist = fabs( point.x() - uv_origin.x() );
					if( dist < bestDist ){
						bestDist = dist;
						snapTo = point.x();
					}
				} );
				Vector3 result( uv_origin_start );
				if( snapper.x_snaps( bestDist ) || snapHard ){
					result.x() = snapTo;
				}
				else{
					result.x() = uv_origin.x();
				}
				m_origin = matrix4_transformed_point( m_faceTex2local, result );
				UpdateFaceData( false, false );
				SceneChangeNotify();
			}
			break;
		case eGridU:
			{
				const Vector3 uv_origin = matrix4_transformed_point( m_faceLocal2tex, m_origin );
				const Vector3 uv_current = matrix4_transformed_point( m_faceLocal2tex, current );

				const float dist = std::max( ( float_to_integer( uv_origin.y() + m_gridSign.x() * .25 ) + m_gridSign.x() - uv_current.y() ) * m_gridSign.x(), .01f );
				unsigned int grid = std::max( 1, std::min( 16, int( 1 / dist ) ) );

				if( snapHard ){ // http://graphics.stanford.edu/~seander/bithacks.html#RoundUpPowerOf2
					grid--;
					grid |= grid >> 1;
					grid |= grid >> 2;
					grid |= grid >> 4;
					grid |= grid >> 8;
					grid |= grid >> 16;
					grid++;
				}

				if( m_gridU != grid || ( snap && m_gridV != grid ) ){
					m_gridU = grid;
					if( snap )
						m_gridV = grid;
					UpdateFaceData( false );
					SceneChangeNotify();
				}
			}
			break;
		case eGridV:
			{
				const Vector3 uv_origin = matrix4_transformed_point( m_faceLocal2tex, m_origin );
				const Vector3 uv_current = matrix4_transformed_point( m_faceLocal2tex, current );

				const float dist = std::max( ( float_to_integer( uv_origin.x() + m_gridSign.y() * .25 ) + m_gridSign.y() - uv_current.x() ) * m_gridSign.y(), .01f );
				unsigned int grid = std::max( 1, std::min( 16, int( 1 / dist ) ) );

				if( snapHard ){ // http://graphics.stanford.edu/~seander/bithacks.html#RoundUpPowerOf2
					grid--;
					grid |= grid >> 1;
					grid |= grid >> 2;
					grid |= grid >> 4;
					grid |= grid >> 8;
					grid |= grid >> 16;
					grid++;
				}

				if( m_gridV != grid || ( snap && m_gridU != grid ) ){
					m_gridV = grid;
					if( snap )
						m_gridU = grid;
					UpdateFaceData( false );
					SceneChangeNotify();
				}
			}
			break;
		case eCircle:
			{
				Vector3 from = m_start - m_origin;
				constrain_to_axis( from, m_tex2local.z().vec3() );
				Vector3 to = current - m_origin;
				constrain_to_axis( to, m_tex2local.z().vec3() );
				Matrix4 rot = g_matrix4_identity;
				if( snap ){
					matrix4_pivoted_rotate_by_axisangle( rot,
					                                     m_tex2local.z().vec3(),
					                                     float_snapped( angle_for_axis( from, to, m_tex2local.z().vec3() ), static_cast<float>( c_pi / 12.0 ) ),
					                                     m_origin );
				}
				else{
					matrix4_pivoted_rotate_by_axisangle( rot,
					                                     m_tex2local.z().vec3(),
					                                     angle_for_axis( from, to, m_tex2local.z().vec3() ),
					                                     m_origin );
				}
				{	// snap
					const Vector3 uvec = vector3_normalised( matrix4_transformed_direction( rot, m_tex2local.x().vec3() ) );
					const Vector3 vvec = vector3_normalised( matrix4_transformed_direction( rot, m_tex2local.y().vec3() ) );
					float bestDot = 0;
					Vector3 bestTo;
					bool V = false;
					forEachEdge( [&]( const Vector3& point0, const Vector3& point1 ){
						Vector3 vec( point1 - point0 );
						constrain_to_axis( vec, m_tex2local.z().vec3() );
						const float dotU = fabs( vector3_dot( uvec, vec ) );
						if( dotU > bestDot ){
							bestDot = dotU;
							bestTo = vector3_dot( uvec, vec ) > 0? vec : -vec;
							V = false;
						}
						const float dotV = fabs( vector3_dot( vvec, vec ) );
						if( dotV > bestDot ){
							bestDot = dotV;
							bestTo = vector3_dot( vvec, vec ) > 0? vec : -vec;
							V = true;
						}
					} );
					if( bestDot > 0.9994f || snapHard ){
						const Vector3 bestFrom = vector3_normalised( V? m_tex2local.y().vec3() : m_tex2local.x().vec3() );
						rot = g_matrix4_identity;
						matrix4_pivoted_rotate_by_axisangle( rot,
						                                     m_tex2local.z().vec3(),
						                                     angle_for_axis( bestFrom, bestTo, m_tex2local.z().vec3() ),
						                                     m_origin );
					}
				}

				Matrix4 faceTex2local = matrix4_multiplied_by_matrix4( rot, m_tex2local );
				faceTex2local.x().vec3() = plane3_project_point( Plane3( m_plane.normal(), 0 ), faceTex2local.x().vec3(), m_tex2local.z().vec3() );
				faceTex2local.y().vec3() = plane3_project_point( Plane3( m_plane.normal(), 0 ), faceTex2local.y().vec3(), m_tex2local.z().vec3() );
				faceTex2local = matrix4_multiplied_by_matrix4( // adjust to have UV's z = 0: move the plane along m_tex2local.z() so that plane.dist() = 0
				                    matrix4_translation_for_vec3(
				                        m_tex2local.z().vec3() * ( m_plane.dist() - vector3_dot( m_plane.normal(), faceTex2local.t().vec3() ) )
				                        / vector3_dot( m_plane.normal(), m_tex2local.z().vec3() )
				                    ),
				                    faceTex2local );
				m_lines2world = m_pivotLines2world = faceTex2local;

				m_pivot2world = matrix4_multiplied_by_matrix4( rot, m_pivot2world0 );

				commitTransform( rot );
			}
			break;
		case eU: //!? todo modifier or default snap to set scale u = scale v
			{
				const Vector3 uv_origin = matrix4_transformed_point( m_local2tex, m_origin );
				const Vector3 uv_start = m_selectedU->vertex;
				const Vector3 uv_current = m_selectedU->vertex + matrix4_transformed_point( m_local2tex, current ) - matrix4_transformed_point( m_local2tex, m_start );
				float bestDist = FLT_MAX;
				float snapTo = 0;
				forEachUVPoint( [&]( const Vector3& point ){
					const float dist = fabs( point.y() - uv_current.y() );
					if( dist < bestDist ){
						bestDist = dist;
						snapTo = point.y();
					}
				} );
				Vector3 result( 1, uv_current.y(), 1 );
				if( snapper.y_snaps( bestDist ) || snapHard ){
					result.y() = snapTo;
				}
				result.y() = ( result.y() - uv_origin.y() ) / ( uv_start.y() - uv_origin.y() );

				if( snap )
					result.x() = fabs( result.y() );

				/* prevent scaling to 0, limit at max 10 textures per world unit */
				if( vector3_length_squared( m_tex2local.y().vec3() * result.y() ) < .01 )
					return;

				Matrix4 scale = g_matrix4_identity;
				matrix4_pivoted_scale_by_vec3( scale, result, uv_origin );
				scale = transform_local2object( scale, m_tex2local, m_local2tex );
				{
					Matrix4 linescale = g_matrix4_identity;
					matrix4_pivoted_scale_by_vec3( linescale, result, matrix4_transformed_point( m_faceLocal2tex, m_origin ) );
					m_lines2world = m_pivotLines2world = matrix4_multiplied_by_matrix4( m_faceTex2local, linescale );

					m_pivot2world = matrix4_multiplied_by_matrix4( m_pivot2world0, matrix4_scale_for_vec3( result ) );
				}
				commitTransform( scale );
			}
			break;
		case eV:
			{
				const Vector3 uv_origin = matrix4_transformed_point( m_local2tex, m_origin );
				const Vector3 uv_start = m_selectedV->vertex;
				const Vector3 uv_current = m_selectedV->vertex + matrix4_transformed_point( m_local2tex, current ) - matrix4_transformed_point( m_local2tex, m_start );
				float bestDist = FLT_MAX;
				float snapTo = 0;
				forEachUVPoint( [&]( const Vector3& point ){
					const float dist = fabs( point.x() - uv_current.x() );
					if( dist < bestDist ){
						bestDist = dist;
						snapTo = point.x();
					}
				} );
				Vector3 result( uv_current.x(), 1, 1 );
				if( snapper.x_snaps( bestDist ) || snapHard ){
					result.x() = snapTo;
				}
				result.x() = ( result.x() - uv_origin.x() ) / ( uv_start.x() - uv_origin.x() );

				if( snap )
					result.y() = fabs( result.x() );

				/* prevent scaling to 0, limit at max 10 textures per world unit */
				if( vector3_length_squared( m_tex2local.x().vec3() * result.x() ) < .01 )
					return;

				Matrix4 scale = g_matrix4_identity;
				matrix4_pivoted_scale_by_vec3( scale, result, uv_origin );
				scale = transform_local2object( scale, m_tex2local, m_local2tex );
				{
					Matrix4 linescale = g_matrix4_identity;
					matrix4_pivoted_scale_by_vec3( linescale, result, matrix4_transformed_point( m_faceLocal2tex, m_origin ) );
					m_lines2world = m_pivotLines2world = matrix4_multiplied_by_matrix4( m_faceTex2local, linescale );

					m_pivot2world = matrix4_multiplied_by_matrix4( m_pivot2world0, matrix4_scale_for_vec3( result ) );
				}
				commitTransform( scale );
			}
			break;
		case eUV:
			{
				const Vector3 uv_origin = matrix4_transformed_point( m_local2tex, m_origin );
				const Vector3 uv_start{ m_selectedV->vertex.x(), m_selectedU->vertex.y(), 0 };
				const Vector3 uv_current{ ( m_selectedV->vertex + matrix4_transformed_point( m_local2tex, current ) - matrix4_transformed_point( m_local2tex, m_start ) ).x(),
				                          ( m_selectedU->vertex + matrix4_transformed_point( m_local2tex, current ) - matrix4_transformed_point( m_local2tex, m_start ) ).y(),
				                          0 };
				float bestDistU = FLT_MAX;
				float snapToU = 0;
				float bestDistV = FLT_MAX;
				float snapToV = 0;
				forEachUVPoint( [&]( const Vector3& point ){
					const float distU = fabs( point.y() - uv_current.y() );
					if( distU < bestDistU ){
						bestDistU = distU;
						snapToU = point.y();
					}
					const float distV = fabs( point.x() - uv_current.x() );
					if( distV < bestDistV ){
						bestDistV = distV;
						snapToV = point.x();
					}
				} );

				Vector3 result( uv_current.x(), uv_current.y(), 1 );
				if( snapper.y_snaps( bestDistU ) || snapHard ){
					result.y() = snapToU;
				}
				result.y() = ( result.y() - uv_origin.y() ) / ( uv_start.y() - uv_origin.y() );

				if( snapper.x_snaps( bestDistV ) || snapHard ){
					result.x() = snapToV;
				}
				result.x() = ( result.x() - uv_origin.x() ) / ( uv_start.x() - uv_origin.x() );

				if( snap ){
					const std::size_t best = fabs( result.x() ) > fabs( result.y() )? 0 : 1;
					result[( best + 1 ) % 2] = std::copysign( result[best], result[( best + 1 ) % 2] );
				}

				/* prevent scaling to 0, limit at max 10 textures per world unit */
				if( vector3_length_squared( m_tex2local.x().vec3() * result.x() ) < .01 ||
				    vector3_length_squared( m_tex2local.y().vec3() * result.y() ) < .01 )
					return;

				Matrix4 scale = g_matrix4_identity;
				matrix4_pivoted_scale_by_vec3( scale, result, uv_origin );
				scale = transform_local2object( scale, m_tex2local, m_local2tex );
				{
					Matrix4 linescale = g_matrix4_identity;
					matrix4_pivoted_scale_by_vec3( linescale, result, matrix4_transformed_point( m_faceLocal2tex, m_origin ) );
					m_lines2world = m_pivotLines2world = matrix4_multiplied_by_matrix4( m_faceTex2local, linescale );

					m_pivot2world = matrix4_multiplied_by_matrix4( m_pivot2world0, matrix4_scale_for_vec3( result ) );
				}
				commitTransform( scale );
			}
			break;
		case eSkewU:
			{
				const Vector3 uv_origin = matrix4_transformed_point( m_faceLocal2tex, m_origin );
				const Vector3 uv_move = matrix4_transformed_point( m_faceLocal2tex, current ) - matrix4_transformed_point( m_faceLocal2tex, m_start );
				Matrix4 skew( g_matrix4_identity );
				skew[4] = uv_move.x() / ( m_selectedU->vertex - uv_origin ).y();

				const Vector3 skewed = matrix4_transformed_direction( skew, g_vector3_axis_y );
				const float uv_y_measure_dist = ( m_selectedU->vertex - uv_origin ).y();
				float bestDist = FLT_MAX;
				Vector3 bestTo;
				const auto snap_to_edge = [&]( const Vector3 edge ){
					if( fabs( edge.y() ) > 1e-5 ){ // don't snap so, that one axis = the other
						const float dist = fabs( edge.x() * uv_y_measure_dist / edge.y() - skewed.x() * uv_y_measure_dist / skewed.y() );
						if( dist < bestDist ){
							bestDist = dist;
							bestTo = edge;
						}
					}
				};
				forEachEdge( [&]( const Vector3& point0, const Vector3& point1 ){
					snap_to_edge( matrix4_transformed_point( m_faceLocal2tex, point1 ) - matrix4_transformed_point( m_faceLocal2tex, point0 ) );
				} );
				forEachPoint( [&]( const Vector3& point ){
					const Vector3 po = matrix4_transformed_point( m_faceLocal2tex, point );
					for( std::vector<PointVertex>::const_iterator i = m_Vlines.m_lines.cbegin(); i != m_Vlines.m_lines.cend(); ++++i ){
						snap_to_edge( po - Vector3( i->vertex.x(), uv_origin.y(), 0 ) );
					}
					snap_to_edge( po - Vector3( uv_origin.x(), uv_origin.y(), 0 ) );
				} );
				if( snapper.x_snaps( bestDist, .015f ) || snapHard ){ //!? todo add snap: make manipulated axis orthogonal to the other
					skew[4] = bestTo.x() / bestTo.y();
				}

				{
					Matrix4 mat( g_matrix4_identity );
					matrix4_translate_by_vec3( mat, uv_origin );
					matrix4_multiply_by_matrix4( mat, skew );
					matrix4_translate_by_vec3( mat, -uv_origin );
					skew = mat;
				}

				m_lines2world = m_pivotLines2world = matrix4_multiplied_by_matrix4( m_faceTex2local, skew );
				m_pivot2world = transform_local2object( skew, m_tex2local, m_local2tex );
				matrix4_multiply_by_matrix4( m_pivot2world, m_pivot2world0 );

				skew = transform_local2object( skew, m_faceTex2local, m_faceLocal2tex );
				commitTransform( skew );
			}
			break;
		case eSkewV:
			{
				const Vector3 uv_origin = matrix4_transformed_point( m_faceLocal2tex, m_origin );
				const Vector3 uv_move = matrix4_transformed_point( m_faceLocal2tex, current ) - matrix4_transformed_point( m_faceLocal2tex, m_start );
				Matrix4 skew( g_matrix4_identity );
				skew[1] = uv_move.y() / ( m_selectedV->vertex - uv_origin ).x();

				const Vector3 skewed = matrix4_transformed_direction( skew, g_vector3_axis_x );
				const float uv_x_measure_dist = ( m_selectedV->vertex - uv_origin ).x();
				float bestDist = FLT_MAX;
				Vector3 bestTo;
				const auto snap_to_edge = [&]( const Vector3 edge ){
					if( fabs( edge.x() ) > 1e-5 ){ // don't snap so, that one axis = the other
						const float dist = fabs( edge.y() * uv_x_measure_dist / edge.x() - skewed.y() * uv_x_measure_dist / skewed.x() );
						if( dist < bestDist ){
							bestDist = dist;
							bestTo = edge;
						}
					}
				};
				forEachEdge( [&]( const Vector3& point0, const Vector3& point1 ){
					snap_to_edge( matrix4_transformed_point( m_faceLocal2tex, point1 ) - matrix4_transformed_point( m_faceLocal2tex, point0 ) );
				} );
				forEachPoint( [&]( const Vector3& point ){
					const Vector3 po = matrix4_transformed_point( m_faceLocal2tex, point );
					for( std::vector<PointVertex>::const_iterator i = m_Ulines.m_lines.cbegin(); i != m_Ulines.m_lines.cend(); ++++i ){
						snap_to_edge( po - Vector3( uv_origin.x(), i->vertex.y(), 0 ) );
					}
					snap_to_edge( po - Vector3( uv_origin.x(), uv_origin.y(), 0 ) );
				} );
				if( snapper.y_snaps( bestDist, .015f ) || snapHard ){ //!? todo add snap: make manipulated axis orthogonal to the other
					skew[1] = bestTo.y() / bestTo.x();
				}

				{
					Matrix4 mat( g_matrix4_identity );
					matrix4_translate_by_vec3( mat, uv_origin );
					matrix4_multiply_by_matrix4( mat, skew );
					matrix4_translate_by_vec3( mat, -uv_origin );
					skew = mat;
				}

				m_lines2world = m_pivotLines2world = matrix4_multiplied_by_matrix4( m_faceTex2local, skew );
				m_pivot2world = transform_local2object( skew, m_tex2local, m_local2tex );
				matrix4_multiply_by_matrix4( m_pivot2world, m_pivot2world0 );

				skew = transform_local2object( skew, m_faceTex2local, m_faceLocal2tex );
				commitTransform( skew );
			}
			break;
		case eTex:
			{
				const Vector3 uvstart = matrix4_transformed_point( m_faceLocal2tex, m_start );
				const Vector3 uvcurrent = matrix4_transformed_point( m_faceLocal2tex, current );
				const Vector3 uvmove = uvcurrent - uvstart;
				float bestDistU = FLT_MAX;
				float bestDistV = FLT_MAX;
				float snapMoveU = 0;
				float snapMoveV = 0;
				// snap uvmove
				const auto functor = [&]( const Vector3& point ){
					for( std::vector<PointVertex>::const_iterator i = m_Ulines.m_lines.begin(); i != m_Ulines.m_lines.end(); ++++i ){
						const float dist = point.y() - ( ( *i ).vertex.y() + uvmove.y() );
						if( fabs( dist ) < bestDistU ){
							bestDistU = fabs( dist );
							snapMoveU = uvmove.y() + dist;
						}
					}
					for( std::vector<PointVertex>::const_iterator i = m_Vlines.m_lines.begin(); i != m_Vlines.m_lines.end(); ++++i ){
						const float dist = point.x() - ( ( *i ).vertex.x() + uvmove.x() );
						if( fabs( dist ) < bestDistV ){
							bestDistV = fabs( dist );
							snapMoveV = uvmove.x() + dist;
						}
					}
				};
				forEachUVPoint( functor );
				functor( matrix4_transformed_point( m_faceLocal2tex, m_origin ) );

				Vector3 result( uvmove );
				if( snapper.y_snaps( bestDistU ) || snapHard ){
					result.y() = snapMoveU;
				}
				if( snapper.x_snaps( bestDistV ) || snapHard ){
					result.x() = snapMoveV;
				}

				if( snap ){
					auto& smaller = fabs( uvmove.x() * vector3_length( m_faceTex2local.x().vec3() ) ) <
					                fabs( uvmove.y() * vector3_length( m_faceTex2local.y().vec3() ) )? result.x() : result.y();
					smaller = 0;
				}

				result = translation_local2object( result, m_faceTex2local, m_faceLocal2tex );

				const Matrix4 translation = matrix4_translation_for_vec3( result );

				m_lines2world = matrix4_multiplied_by_matrix4( translation, m_faceTex2local );

				commitTransform( translation );
			}
			break;
		case ePatchPoint:
		case ePatchRow:
		case ePatchColumn:
			{
				std::vector<std::size_t> indices;
				if( m_selection == ePatchPoint )
					indices.push_back( m_selectedPatchIndex );
				else if( m_selection == ePatchRow )
					for ( std::size_t c = 0; c < m_patchWidth; ++c )
						indices.push_back( m_selectedPatchIndex * m_patchWidth + c );
				else if( m_selection == ePatchColumn )
					for ( std::size_t r = 0; r < m_patchHeight; ++r )
						indices.push_back( r * m_patchWidth + m_selectedPatchIndex );

				const Vector3 uvstart = matrix4_transformed_point( m_faceLocal2tex, m_start );
				const Vector3 uvcurrent = matrix4_transformed_point( m_faceLocal2tex, current );
				const Vector3 uvmove = uvcurrent - uvstart;
				float bestDistU = FLT_MAX;
				float bestDistV = FLT_MAX;
				float snapMoveU = 0;
				float snapMoveV = 0;
				// snap uvmove
				for( std::size_t index : indices ){
					for( std::vector<PointVertex>::const_iterator i = m_Ulines.m_lines.begin(); i != m_Ulines.m_lines.end(); ++++i ){
						const float dist = m_patchCtrl[index].m_texcoord.y() + uvmove.y() - ( *i ).vertex.y();
						if( fabs( dist ) < bestDistU ){
							bestDistU = fabs( dist );
							snapMoveU = uvmove.y() - dist;
						}
					}
					for( std::vector<PointVertex>::const_iterator i = m_Vlines.m_lines.begin(); i != m_Vlines.m_lines.end(); ++++i ){
						const float dist = m_patchCtrl[index].m_texcoord.x() + uvmove.x() - ( *i ).vertex.x();
						if( fabs( dist ) < bestDistV ){
							bestDistV = fabs( dist );
							snapMoveV = uvmove.x() - dist;
						}
					}
					const Vector3 origin = matrix4_transformed_point( m_faceLocal2tex, m_origin );
					{
						const float dist = m_patchCtrl[index].m_texcoord.y() + uvmove.y() - origin.y();
						if( fabs( dist ) < bestDistU ){
							bestDistU = fabs( dist );
							snapMoveU = uvmove.y() - dist;
						}
					}
					{
						const float dist = m_patchCtrl[index].m_texcoord.x() + uvmove.x() - origin.x();
						if( fabs( dist ) < bestDistV ){
							bestDistV = fabs( dist );
							snapMoveV = uvmove.x() - dist;
						}
					}
				}

				Vector3 result( uvmove );
				if( snapper.y_snaps( bestDistU ) || snapHard ){
					result.y() = snapMoveU;
				}
				if( snapper.x_snaps( bestDistV ) || snapHard ){
					result.x() = snapMoveV;
				}

				if( snap ){
					auto& smaller = fabs( uvmove.x() * vector3_length( m_faceTex2local.x().vec3() ) ) <
					                fabs( uvmove.y() * vector3_length( m_faceTex2local.y().vec3() ) )? result.x() : result.y();
					smaller = 0;
				}

				const Matrix4 translation = matrix4_translation_for_vec3( result );
				for( std::size_t i : indices ){
					const Vector3 uv = matrix4_transformed_point( translation, Vector3( m_patchCtrl[i].m_texcoord ) );
					m_patch->getControlPointsTransformed()[i].m_texcoord = uv.vec2();
					m_patchRenderPoints.m_points[i].vertex = vertex3f_for_vector3( uv );
				}

				// update lattice renderable entirely
				for ( std::size_t r = 0; r < m_patchHeight; ++r ){
					for ( std::size_t c = 0; c < m_patchWidth - 1; ++c ){
						const Vector2& a = m_patch->getControlPointsTransformed()[r * m_patchWidth + c].m_texcoord;
						const Vector2& b = m_patch->getControlPointsTransformed()[r * m_patchWidth + c + 1].m_texcoord;
						m_patchRenderLattice.m_lines[( r * ( m_patchWidth - 1 ) + c ) * 2].vertex = vertex3f_for_vector3( Vector3( a ) );
						m_patchRenderLattice.m_lines[( r * ( m_patchWidth - 1 ) + c ) * 2 + 1].vertex = vertex3f_for_vector3( Vector3( b ) );
					}
				}
				for ( std::size_t c = 0; c < m_patchWidth; ++c ){
					for ( std::size_t r = 0; r < m_patchHeight - 1; ++r ){
						const Vector2& a = m_patch->getControlPointsTransformed()[r * m_patchWidth + c].m_texcoord;
						const Vector2& b = m_patch->getControlPointsTransformed()[( r + 1 ) * m_patchWidth + c].m_texcoord;
						m_patchRenderLattice.m_lines[( m_patchWidth - 1 ) * m_patchHeight * 2 + ( c * ( m_patchHeight - 1 ) + r ) * 2].vertex = vertex3f_for_vector3( Vector3( a ) );
						m_patchRenderLattice.m_lines[( m_patchWidth - 1 ) * m_patchHeight * 2 + ( c * ( m_patchHeight - 1 ) + r ) * 2 + 1].vertex = vertex3f_for_vector3( Vector3( b ) );
					}
				}

				m_patch->UpdateCachedData();
				SceneChangeNotify();
			}
		default:
			break;
		}
	}

	void freezeTransform(){
		if( m_selection == eCircle
		 || m_selection == eU
		 || m_selection == eV
		 || m_selection == eUV
		 || m_selection == eSkewU
		 || m_selection == eSkewV
		 || m_selection == eTex
		 || m_selection == ePatchPoint
		 || m_selection == ePatchRow
		 || m_selection == ePatchColumn )
		{
			if( m_face ){
				m_face->freezeTransform();
				Brush_textureChanged();
			}
			else if( m_patch ){
				m_patch->freezeTransform();
				Patch_textureChanged();
			}
		}
	}

	Manipulatable* GetManipulatable() override {
		return this;
	}

	void setSelected( bool select ) override {
		m_isSelected = select;
	}
	bool isSelected() const override {
		return m_isSelected;
	}
};

Shader* UVManipulator::m_state_line;
Shader* UVManipulator::m_state_point;




class TransformOriginTranslatable
{
public:
	virtual void transformOriginTranslate( const Vector3& translation, const bool set[3] ) = 0;
};

class TransformOriginTranslate : public Manipulatable
{
private:
	Vector3 m_start;
	TransformOriginTranslatable& m_transformOriginTranslatable;
public:
	TransformOriginTranslate( TransformOriginTranslatable& transformOriginTranslatable )
		: m_transformOriginTranslatable( transformOriginTranslatable ){
	}
	void Construct( const Matrix4& device2manip, const DeviceVector device_point, const AABB& bounds, const Vector3& transform_origin ) override {
		m_start = point_on_plane( device2manip, device_point );
	}
	void Transform( const Matrix4& manip2object, const Matrix4& device2manip, const DeviceVector device_point ) override {
		Vector3 current = point_on_plane( device2manip, device_point );
		current = vector3_subtracted( current, m_start );

		if( g_modifiers.shift() ){ // snap to axis
			for ( std::size_t i = 0; i < 3; ++i ){
				if( fabs( current[i] ) >= fabs( current[( i + 1 ) % 3] ) ){
					current[( i + 1 ) % 3] = 0.f;
				}
				else{
					current[i] = 0.f;
				}
			}
		}

		bool set[3] = { true, true, true };
		for ( std::size_t i = 0; i < 3; ++i ){
			if( fabs( current[i] ) < 1e-3f ){
				set[i] = false;
			}
		}

		current = translation_local2object( current, manip2object );

		m_transformOriginTranslatable.transformOriginTranslate( current, set );
	}
};

class TransformOriginManipulator : public Manipulator, public ManipulatorSelectionChangeable
{
	struct RenderablePoint : public OpenGLRenderable
	{
		PointVertex m_point;
		RenderablePoint():
			m_point( vertex3f_identity ) {
		}
		void render( RenderStateFlags state ) const override {
			gl().glColorPointer( 4, GL_UNSIGNED_BYTE, sizeof( PointVertex ), &m_point.colour );
			gl().glVertexPointer( 3, GL_FLOAT, sizeof( PointVertex ), &m_point.vertex );
			gl().glDrawArrays( GL_POINTS, 0, 1 );
		}
		void setColour( const Colour4b & colour ) {
			m_point.colour = colour;
		}
	};

	TransformOriginTranslate m_translate;
	const bool& m_pivotIsCustom;
	RenderablePoint m_point;
	SelectableBool m_selectable;
	Pivot2World m_pivot;
public:
	static Shader* m_state;

	TransformOriginManipulator( TransformOriginTranslatable& transformOriginTranslatable, const bool& pivotIsCustom ) :
		m_translate( transformOriginTranslatable ),
		m_pivotIsCustom( pivotIsCustom ){
	}

	void UpdateColours() {
		m_point.setColour(
			m_selectable.isSelected()?
				m_pivotIsCustom? Colour4b( 255, 232, 0, 255 )
				: g_colour_selected
			:	m_pivotIsCustom? Colour4b( 0, 125, 255, 255 )
				: g_colour_screen );
	}

	void render( Renderer& renderer, const VolumeTest& volume, const Matrix4& pivot2world ) override {
		m_pivot.update( pivot2world, volume.GetModelview(), volume.GetProjection(), volume.GetViewport() );

		// temp hack
		UpdateColours();

		renderer.SetState( m_state, Renderer::eWireframeOnly );
		renderer.SetState( m_state, Renderer::eFullMaterials );

		renderer.addRenderable( m_point, m_pivot.m_worldSpace );
	}
	void testSelect( const View& view, const Matrix4& pivot2world ) override {
		if( g_modifiers != c_modifierNone )
			return selectionChange( nullptr );

		m_pivot.update( pivot2world, view.GetModelview(), view.GetProjection(), view.GetViewport() );

		SelectionPool selector;
		{
			const Matrix4 local2view( matrix4_multiplied_by_matrix4( view.GetViewMatrix(), m_pivot.m_worldSpace ) );

#if defined( DEBUG_SELECTION )
			g_render_clipped.construct( view.GetViewMatrix() );
#endif
			{
				SelectionIntersection best;

				Point_BestPoint( local2view, m_point.m_point, best );
				selector.addSelectable( best, &m_selectable );
			}
		}

		selectionChange( selector );
	}

	Manipulatable* GetManipulatable() override {
		return &m_translate;
	}

	void setSelected( bool select ) override {
		m_selectable.setSelected( select );
	}
	bool isSelected() const override {
		return m_selectable.isSelected();
	}
};
Shader* TransformOriginManipulator::m_state;

class TransformsObserved : public Transforms
{
public:
	void setTranslation( const Translation& value ){
		Transforms::setTranslation( value );
		m_changedCallbacks[SelectionSystem::eTranslate]( StringStream<64>( m_translation == c_translation_identity? ' ' : 'x',
			" Translate ", m_translation.x(), ' ', m_translation.y(), ' ', m_translation.z() ) );
	}
	void setRotation( const Rotation& value ){
		Transforms::setRotation( value );
		m_changedCallbacks[SelectionSystem::eRotate]( StringStream<64>( m_rotation == c_rotation_identity? ' ' : 'x',
			" Rotate ", m_rotation.x(), ' ', m_rotation.y(), ' ', m_rotation.z() ) );
	}
	void setScale( const Scale& value ){
		Transforms::setScale( value );
		m_changedCallbacks[SelectionSystem::eScale]( StringStream<64>( m_scale == c_scale_identity? ' ' : 'x',
			" Scale ", m_scale.x(), ' ', m_scale.y(), ' ', m_scale.z() ) );
	}
	void setSkew( const Skew& value ){
		Transforms::setSkew( value );
		m_changedCallbacks[SelectionSystem::eSkew]( StringStream<64>( m_skew == c_skew_identity? ' ' : 'x',
			" Skew ", m_skew.index, ' ', m_skew.amount ) );
	}

	std::array<Callback<void(const char*)>, 4> m_changedCallbacks;
	static_assert( SelectionSystem::eTranslate == 0
	            && SelectionSystem::eRotate == 1
				&& SelectionSystem::eScale == 2
				&& SelectionSystem::eSkew == 3 );
};

class select_all : public scene::Graph::Walker
{
	bool m_select;
public:
	select_all( bool select )
		: m_select( select ){
	}
	bool pre( const scene::Path& path, scene::Instance& instance ) const override {
		Selectable* selectable = Instance_getSelectable( instance );
		if ( selectable != 0 ) {
			selectable->setSelected( m_select );
		}
		return true;
	}
};

class select_all_component : public scene::Graph::Walker
{
	bool m_select;
	SelectionSystem::EComponentMode m_mode;
public:
	select_all_component( bool select, SelectionSystem::EComponentMode mode )
		: m_select( select ), m_mode( mode ){
	}
	bool pre( const scene::Path& path, scene::Instance& instance ) const override {
		ComponentSelectionTestable* componentSelectionTestable = Instance_getComponentSelectionTestable( instance );
		if ( componentSelectionTestable ) {
			componentSelectionTestable->setSelectedComponents( m_select, m_mode );
		}
		return true;
	}
};

void Scene_SelectAll_Component( bool select, SelectionSystem::EComponentMode componentMode ){
	GlobalSceneGraph().traverse( select_all_component( select, componentMode ) );
}

void Scene_BoundsSelected( scene::Graph& graph, AABB& bounds );
class LazyBounds
{
	AABB m_bounds;
	bool m_valid;
public:
	LazyBounds() : m_valid( false ){
	}
	void setInvalid(){
		m_valid = false;
	}
	const AABB& getBounds(){
		if( !m_valid ){
			Scene_BoundsSelected( GlobalSceneGraph(), m_bounds );
			m_valid = true;
		}
		return m_bounds;
	}
};


// RadiantSelectionSystem
class RadiantSelectionSystem final :
	public SelectionSystem,
	public Translatable,
	public Rotatable,
	public Scalable,
	public Skewable,
	public AllTransformable,
	public TransformOriginTranslatable,
	public Renderable
{
	mutable Matrix4 m_pivot2world;
	mutable AABB m_bounds;
	mutable LazyBounds m_lazy_bounds;
	Matrix4 m_pivot2world_start;
	Matrix4 m_manip2pivot_start;
	Translation m_translation;
	Rotation m_rotation;
	Scale m_scale;
	Skew m_skew;
public:
	static Shader* m_state;
	bool m_bPreferPointEntsIn2D;
private:
	EManipulatorMode m_manipulator_mode;
	Manipulator* m_manipulator;

// state
	bool m_undo_begun;
	EMode m_mode;
	EComponentMode m_componentmode;

	SelectionCounter m_count_primitive;
	SelectionCounter m_count_component;
	SelectedStuffCounter m_count_stuff;

	TranslateManipulator m_translate_manipulator;
	RotateManipulator m_rotate_manipulator;
	ScaleManipulator m_scale_manipulator;
	SkewManipulator m_skew_manipulator;
	DragManipulator m_drag_manipulator;
	ClipManipulator m_clip_manipulator;
	BuildManipulator m_build_manipulator;
	UVManipulator m_uv_manipulator;
	mutable TransformOriginManipulator m_transformOrigin_manipulator;

	typedef SelectionList<scene::Instance> selection_t;
	selection_t m_selection;
	selection_t m_component_selection;

	Signal1<const Selectable&> m_selectionChanged_callbacks;

	void ConstructPivot() const;
	void ConstructPivotRotation() const;
	void setCustomTransformOrigin( const Vector3& origin, const bool set[3] ) const override;
	AABB getSelectionAABB() const;
	mutable bool m_pivotChanged;
	bool m_pivot_moving;
	mutable bool m_pivotIsCustom;

	void Scene_TestSelect( Selector& selector, SelectionTest& test, const View& view, SelectionSystem::EMode mode, SelectionSystem::EComponentMode componentMode );

	bool nothingSelected() const {
		return ( Mode() == eComponent && m_count_component.empty() )
		    || ( Mode() == ePrimitive && m_count_primitive.empty() );
	}


public:
	enum EModifier
	{
		eManipulator,
		eReplace,
		eCycle,
		eSelect,
		eDeselect,
	};

	RadiantSelectionSystem() :
		m_bPreferPointEntsIn2D( true ),
		m_undo_begun( false ),
		m_mode( ePrimitive ),
		m_componentmode( eDefault ),
		m_count_primitive( SelectionChangedCaller( *this ) ),
		m_count_component( SelectionChangedCaller( *this ) ),
		m_translate_manipulator( *this, 2, 64 ),
		m_rotate_manipulator( *this, 8, 64 ),
		m_scale_manipulator( *this, 0, 64 ),
		m_skew_manipulator( *this, *this, *this, *this, *this, m_bounds, m_pivot2world, m_pivotIsCustom ),
		m_drag_manipulator( *this, *this ),
		m_clip_manipulator( m_pivot2world, m_bounds ),
		m_transformOrigin_manipulator( *this, m_pivotIsCustom ),
		m_pivotChanged( false ),
		m_pivot_moving( false ),
		m_pivotIsCustom( false ){
		SetManipulatorMode( eTranslate );
		pivotChanged();
		addSelectionChangeCallback( PivotChangedSelectionCaller( *this ) );
		AddGridChangeCallback( PivotChangedCaller( *this ) );
	}
	void pivotChanged() const override {
		m_pivotChanged = true;
		m_lazy_bounds.setInvalid();
		SceneChangeNotify();
	}
	typedef ConstMemberCaller<RadiantSelectionSystem, void(), &RadiantSelectionSystem::pivotChanged> PivotChangedCaller;
	void pivotChangedSelection( const Selectable& selectable ){
		pivotChanged();
	}
	typedef MemberCaller<RadiantSelectionSystem, void(const Selectable&), &RadiantSelectionSystem::pivotChangedSelection> PivotChangedSelectionCaller;

	const AABB& getBoundsSelected() const override {
		return m_lazy_bounds.getBounds();
	}

	void SetMode( EMode mode ) override {
		if ( m_mode != mode ) {
			m_mode = mode;
			pivotChanged();
		}
	}
	EMode Mode() const override {
		return m_mode;
	}
	void SetComponentMode( EComponentMode mode ) override {
		m_componentmode = mode;
	}
	EComponentMode ComponentMode() const override {
		return m_componentmode;
	}
	void SetManipulatorMode( EManipulatorMode mode ) override {
		if( ( mode == eClip ) || ( ManipulatorMode() == eClip ) ){
			m_clip_manipulator.reset( ( mode == eClip ) && ( ManipulatorMode() != eClip ) );
			if( ( mode == eClip ) != ( ManipulatorMode() == eClip ) )
				Clipper_modeChanged( mode == eClip );
		}

		m_pivotIsCustom = false;
		m_manipulator_mode = mode;
		switch ( m_manipulator_mode )
		{
		case eTranslate: m_manipulator = &m_translate_manipulator; break;
		case eRotate: m_manipulator = &m_rotate_manipulator; break;
		case eScale: m_manipulator = &m_scale_manipulator; break;
		case eSkew: m_manipulator = &m_skew_manipulator; break;
		case eDrag: m_manipulator = &m_drag_manipulator; break;
		case eClip: m_manipulator = &m_clip_manipulator; resetTransforms( eClip ); break;
		case eBuild:
			{
				m_build_manipulator.initialise();
				m_manipulator = &m_build_manipulator; break;
			}
		case eUV: m_manipulator = &m_uv_manipulator; break;
		}
		pivotChanged();
	}
	EManipulatorMode ManipulatorMode() const override {
		return m_manipulator_mode;
	}

	SelectionChangeCallback getObserver( EMode mode ) override {
		if ( mode == ePrimitive ) {
			return makeCallback( m_count_primitive );
		}
		else
		{
			return makeCallback( m_count_component );
		}
	}
	std::size_t countSelected() const override {
		return m_count_primitive.size();
	}
	std::size_t countSelectedComponents() const override {
		return m_count_component.size();
	}
	void countSelectedStuff( std::size_t& brushes, std::size_t& patches, std::size_t& entities ) const override {
		m_count_stuff.get( brushes, patches, entities );
	}
	void onSelectedChanged( scene::Instance& instance, const Selectable& selectable ) override {
		if ( selectable.isSelected() ) {
			m_selection.append( instance );
			m_count_stuff.increment( instance.path().top() );
		}
		else
		{
			m_selection.erase( instance );
			m_count_stuff.decrement( instance.path().top() );
		}

		ASSERT_MESSAGE( m_selection.size() == m_count_primitive.size(), "selection-tracking error" );
	}
	void onComponentSelection( scene::Instance& instance, const Selectable& selectable ) override {
		if ( selectable.isSelected() ) {
			m_component_selection.append( instance );
		}
		else
		{
			m_component_selection.erase( instance );
		}

		ASSERT_MESSAGE( m_component_selection.size() == m_count_component.size(), "selection-tracking error" );
	}
	scene::Instance& firstSelected() const override {
		ASSERT_MESSAGE( m_selection.size() > 0, "no instance selected" );
		return **m_selection.begin();
	}
	scene::Instance& ultimateSelected() const override {
		ASSERT_MESSAGE( m_selection.size() > 0, "no instance selected" );
		return m_selection.back();
	}
	scene::Instance& penultimateSelected() const override {
		ASSERT_MESSAGE( m_selection.size() > 1, "only one instance selected" );
		return *( *( --( --m_selection.end() ) ) );
	}
	void setSelectedAll( bool selected ) override {
		GlobalSceneGraph().traverse( select_all( selected ) );

		m_manipulator->setSelected( selected );
	}
	void setSelectedAllComponents( bool selected ) override {
		Scene_SelectAll_Component( selected, SelectionSystem::eVertex );
		Scene_SelectAll_Component( selected, SelectionSystem::eEdge );
		Scene_SelectAll_Component( selected, SelectionSystem::eFace );

		m_manipulator->setSelected( selected );
	}

	void foreachSelected( const Visitor& visitor ) const override {
		selection_t::const_iterator i = m_selection.begin();
		while ( i != m_selection.end() )
		{
			visitor.visit( *( *( i++ ) ) );
		}
	}
	void foreachSelectedComponent( const Visitor& visitor ) const override {
		selection_t::const_iterator i = m_component_selection.begin();
		while ( i != m_component_selection.end() )
		{
			visitor.visit( *( *( i++ ) ) );
		}
	}

	void addSelectionChangeCallback( const SelectionChangeHandler& handler ) override {
		m_selectionChanged_callbacks.connectLast( handler );
	}
	void selectionChanged( const Selectable& selectable ){
		m_selectionChanged_callbacks( selectable );
	}
	typedef MemberCaller<RadiantSelectionSystem, void(const Selectable&), &RadiantSelectionSystem::selectionChanged> SelectionChangedCaller;


	void startMove(){
		m_pivot2world_start = GetPivot2World();
	}

	bool SelectManipulator( const View& view, const DeviceVector device_point, const DeviceVector device_epsilon ){
		bool movingOrigin = false;

		if ( !nothingSelected()
		|| ManipulatorMode() == eDrag
		|| ManipulatorMode() == eClip
		|| ManipulatorMode() == eBuild
		|| ManipulatorMode() == eUV ) {
#if defined ( DEBUG_SELECTION )
			g_render_clipped.destroy();
#endif
			Manipulatable::assign_static( view, device_point, device_epsilon ); //this b4 m_manipulator calls!

			m_transformOrigin_manipulator.setSelected( false );
			m_manipulator->setSelected( false );

			{
				View scissored( view );
				ConstructSelectionTest( scissored, SelectionBoxForPoint( device_point, device_epsilon ) );

				if( transformOrigin_isTranslatable() ){
					m_transformOrigin_manipulator.testSelect( scissored, GetPivot2World() );
					movingOrigin = m_transformOrigin_manipulator.isSelected();
				}

				if( !movingOrigin )
					m_manipulator->testSelect( scissored, GetPivot2World() );
			}

			startMove();

			m_pivot_moving = m_manipulator->isSelected();

			if ( m_pivot_moving || movingOrigin ) {
				Pivot2World pivot;
				pivot.update( GetPivot2World(), view.GetModelview(), view.GetProjection(), view.GetViewport() );

				m_manip2pivot_start = matrix4_multiplied_by_matrix4( matrix4_full_inverse( m_pivot2world_start ), pivot.m_worldSpace );

				Matrix4 device2manip;
				ConstructDevice2Manip( device2manip, m_pivot2world_start, view.GetModelview(), view.GetProjection(), view.GetViewport() );
				if( m_pivot_moving ){
					m_manipulator->GetManipulatable()->Construct( device2manip, device_point, m_bounds, GetPivot2World().t().vec3() );
					m_undo_begun = false;
				}
				else if( movingOrigin ){
					m_transformOrigin_manipulator.GetManipulatable()->Construct( device2manip, device_point, m_bounds, GetPivot2World().t().vec3() );
				}
			}

			SceneChangeNotify();
		}

		return m_pivot_moving || movingOrigin;
	}

	void HighlightManipulator( const View& view, const DeviceVector device_point, const DeviceVector device_epsilon ){
		Manipulatable::assign_static( view, device_point, device_epsilon ); //this b4 m_manipulator calls!

		if ( ( !nothingSelected() && transformOrigin_isTranslatable() )
		     || ManipulatorMode() == eDrag
		     || ManipulatorMode() == eClip
		     || ManipulatorMode() == eBuild
		     || ManipulatorMode() == eUV ) {
#if defined ( DEBUG_SELECTION )
			g_render_clipped.destroy();
#endif

			m_transformOrigin_manipulator.setSelected( false );
			m_manipulator->setSelected( false );

			View scissored( view );
			ConstructSelectionTest( scissored, SelectionBoxForPoint( device_point, device_epsilon ) );

			if( transformOrigin_isTranslatable() ){
				m_transformOrigin_manipulator.testSelect( scissored, GetPivot2World() );

				if( !m_transformOrigin_manipulator.isSelected() )
					m_manipulator->testSelect( scissored, GetPivot2World() );
			}
			else if( ManipulatorMode() == eClip ){
				m_clip_manipulator.testSelect_points( scissored );
			}
			else if( ManipulatorMode() == eBuild ){
				m_build_manipulator.highlight( scissored );
			}
			else if( ManipulatorMode() == eUV ){
				m_manipulator->testSelect( scissored, GetPivot2World() );
			}
			else if( ManipulatorMode() == eDrag ){
				m_drag_manipulator.highlight( scissored );
			}
		}
	}

	void deselectAll(){
		if ( Mode() == eComponent ) {
			setSelectedAllComponents( false );
		}
		else
		{
			setSelectedAll( false );
		}
	}

	void deselectComponentsOrAll( bool components ){
		if ( components ) {
			setSelectedAllComponents( false );
		}
		else
		{
			deselectAll();
		}
	}
#define SELECT_MATCHING
#define SELECT_MATCHING_DEPTH 1e-6f
#define SELECT_MATCHING_DIST 1e-6f
#define SELECT_MATCHING_COMPONENTS_DIST .25f
	void SelectionPool_Select( SelectionPool& pool, bool select, float dist_epsilon ){
		SelectionPool::iterator best = pool.begin();
		if( best->second->isSelected() != select ){
			best->second->setSelected( select );
		}
#ifdef SELECT_MATCHING
		for ( SelectionPool::iterator i = std::next( best ); i != pool.end(); ++i )
		{
			if( i->first.equalEpsilon( best->first, dist_epsilon, SELECT_MATCHING_DEPTH ) ){
				//if( i->second->isSelected() != select ){
				i->second->setSelected( select );
				//}
			}
			else{
				break;
			}
		}
#endif // SELECT_MATCHING
	}

	void SelectPoint( const View& view, const DeviceVector device_point, const DeviceVector device_epsilon, RadiantSelectionSystem::EModifier modifier, bool face ){
		//globalOutputStream() << device_point[0] << "   " << device_point[1] << '\n';
		ASSERT_MESSAGE( fabs( device_point[0] ) <= 1.f && fabs( device_point[1] ) <= 1.f, "point-selection error" );

		if ( modifier == eReplace ) {
			deselectComponentsOrAll( face );
		}
		/*
		//nothingSelected() doesn't consider faces, selected in non-component mode, m
		if ( modifier == eCycle && nothingSelected() ){
			modifier = eReplace;
		}
		*/
#if defined ( DEBUG_SELECTION )
		g_render_clipped.destroy();
#endif

		{
			View scissored( view );
			ConstructSelectionTest( scissored, SelectionBoxForPoint( device_point, device_epsilon ) );

			SelectionVolume volume( scissored );
			SelectionPool selector;
			const bool prefer_point_ents = m_bPreferPointEntsIn2D && Mode() == ePrimitive && !view.fill() && !face
			                               && ( modifier == EModifier::eReplace || modifier == EModifier::eSelect || modifier == EModifier::eDeselect );

			if( prefer_point_ents && ( Scene_TestSelect( selector, volume, scissored, eEntity, ComponentMode() ), !selector.failed() ) ){
				switch ( modifier )
				{
				// if cycle mode not enabled, enable it
				case EModifier::eReplace:
					{
						// select closest
						selector.begin()->second->setSelected( true );
					}
					break;
				case EModifier::eSelect:
					{
						SelectionPool_Select( selector, true, SELECT_MATCHING_DIST );
					}
					break;
				case EModifier::eDeselect:
					{
						SelectionPool_Select( selector, false, SELECT_MATCHING_DIST );
					}
					break;
				default:
					break;
				}
			}
			else{
				const EMode mode = g_modifiers == c_modifierAlt? ePrimitive : Mode();
				if ( face ){
					Scene_TestSelect_Component( selector, volume, scissored, eFace );
				}
				else{
					Scene_TestSelect( selector, volume, scissored, mode, ComponentMode() );
				}

				if ( !selector.failed() ) {
					switch ( modifier )
					{
					// if cycle mode not enabled, enable it
					case EModifier::eReplace:
						{
							// select closest
							selector.begin()->second->setSelected( true );
						}
						break;
					// select the next object in the list from the one already selected
					case EModifier::eCycle:
						{
							bool cycleSelectionOccured = false;
							for ( SelectionPool::iterator i = selector.begin(); i != selector.end(); ++i )
							{
								if ( i->second->isSelected() ) {
									deselectComponentsOrAll( face );
									++i;
									if ( i != selector.end() ) {
										i->second->setSelected( true );
									}
									else
									{
										selector.begin()->second->setSelected( true );
									}
									cycleSelectionOccured = true;
									break;
								}
							}
							if( !cycleSelectionOccured ){
								deselectComponentsOrAll( face );
								selector.begin()->second->setSelected( true );
							}
						}
						break;
					case EModifier::eSelect:
						{
							SelectionPool_Select( selector, true, mode == eComponent? SELECT_MATCHING_COMPONENTS_DIST : SELECT_MATCHING_DIST );
						}
						break;
					case EModifier::eDeselect:
						{
							if( !( mode == ePrimitive && Mode() == eComponent && countSelected() == 1 ) ) // don't deselect only primitive in component mode
								SelectionPool_Select( selector, false, mode == eComponent? SELECT_MATCHING_COMPONENTS_DIST : SELECT_MATCHING_DIST );
						}
						break;
					default:
						break;
					}
				}
				else if( modifier == eCycle ){
					deselectComponentsOrAll( face );
				}
			}
		}
	}

	RadiantSelectionSystem::EModifier
	SelectPoint_InitPaint( const View& view, const DeviceVector device_point, const DeviceVector device_epsilon, bool face ){
		ASSERT_MESSAGE( fabs( device_point[0] ) <= 1.f && fabs( device_point[1] ) <= 1.f, "point-selection error" );
#if defined ( DEBUG_SELECTION )
		g_render_clipped.destroy();
#endif

		{
			View scissored( view );
			ConstructSelectionTest( scissored, SelectionBoxForPoint( device_point, device_epsilon ) );

			SelectionVolume volume( scissored );
			SelectionPool selector;
			const bool prefer_point_ents = m_bPreferPointEntsIn2D && Mode() == ePrimitive && !view.fill() && !face;

			if( prefer_point_ents && ( Scene_TestSelect( selector, volume, scissored, eEntity, ComponentMode() ), !selector.failed() ) ){
				const bool wasSelected = selector.begin()->second->isSelected();
				SelectionPool_Select( selector, !wasSelected, SELECT_MATCHING_DIST );
				return wasSelected? eDeselect : eSelect;
			}
			else{//do primitives, if ents failed
				const EMode mode = g_modifiers == c_modifierAlt? ePrimitive : Mode();
				if ( face ){
					Scene_TestSelect_Component( selector, volume, scissored, eFace );
				}
				else{
					Scene_TestSelect( selector, volume, scissored, mode, ComponentMode() );
				}
				if ( !selector.failed() ){
					const bool wasSelected = selector.begin()->second->isSelected();
					if( !( mode == ePrimitive && Mode() == eComponent && countSelected() == 1 && wasSelected ) ) // don't deselect only primitive in component mode
						SelectionPool_Select( selector, !wasSelected, mode == eComponent? SELECT_MATCHING_COMPONENTS_DIST : SELECT_MATCHING_DIST );

#if 0
					SelectionPool::iterator best = selector.begin();
					SelectionPool::iterator i = best;
					globalOutputStream() << "\n\n\n===========\n";
					while ( i != selector.end() )
					{
						globalOutputStream() << "depth:" << ( *i ).first.m_depth << " dist:" << ( *i ).first.m_distance << " depth2:" << ( *i ).first.m_depth2 << '\n';
						globalOutputStream() << "depth - best depth:" << ( *i ).first.m_depth - ( *best ).first.m_depth << '\n';
						++i;
					}
#endif

					return wasSelected? eDeselect : eSelect;
				}
				else{
					return eSelect;
				}
			}
		}
	}

	void SelectArea( const View& view, const rect_t rect, bool face ){
#if defined ( DEBUG_SELECTION )
		g_render_clipped.destroy();
#endif
		View scissored( view );
		ConstructSelectionTest( scissored, rect );

		SelectionVolume volume( scissored );
		SelectionPool pool;
		if ( face ) {
			Scene_TestSelect_Component( pool, volume, scissored, eFace );
		}
		else
		{
			Scene_TestSelect( pool, volume, scissored, Mode(), ComponentMode() );
		}

		for ( SelectionPool::iterator i = pool.begin(); i != pool.end(); ++i )
		{
			( *i ).second->setSelected( rect.modifier == rect_t::eSelect? true : rect.modifier == rect_t::eDeselect? false : !( *i ).second->isSelected() );
		}
	}


	void translate( const Vector3& translation ) override {
		if ( !nothingSelected() ) {
			//ASSERT_MESSAGE( !m_pivotChanged, "pivot is invalid" );

			m_translation = translation;
			m_repeatableTransforms.setTranslation( translation );

			m_pivot2world = m_pivot2world_start;
			matrix4_translate_by_vec3( m_pivot2world, translation );

			if ( Mode() == eComponent ) {
				Scene_Translate_Component_Selected( GlobalSceneGraph(), m_translation );
			}
			else
			{
				Scene_Translate_Selected( GlobalSceneGraph(), m_translation );
			}

			SceneChangeNotify();
		}
	}
	void outputTranslation( TextOutputStream& ostream ){
		ostream << " -xyz " << m_translation.x() << ' ' << m_translation.y() << ' ' << m_translation.z();
	}
	void rotate( const Quaternion& rotation ) override {
		if ( !nothingSelected() ) {
			//ASSERT_MESSAGE( !m_pivotChanged, "pivot is invalid" );

			m_rotation = rotation;
			m_repeatableTransforms.setRotation( rotation );

			if ( Mode() == eComponent ) {
				Scene_Rotate_Component_Selected( GlobalSceneGraph(), m_rotation, m_pivot2world.t().vec3() );

				matrix4_assign_rotation_for_pivot( m_pivot2world, m_component_selection.back() );
			}
			else
			{
				Scene_Rotate_Selected( GlobalSceneGraph(), m_rotation, m_pivot2world.t().vec3() );

				matrix4_assign_rotation_for_pivot( m_pivot2world, m_selection.back() );
			}
#ifdef SELECTIONSYSTEM_AXIAL_PIVOTS
			matrix4_assign_rotation( m_pivot2world, matrix4_rotation_for_quaternion_quantised( m_rotation ) );
#endif

			SceneChangeNotify();
		}
	}
	void outputRotation( TextOutputStream& ostream ){
		ostream << " -eulerXYZ " << m_rotation.x() << ' ' << m_rotation.y() << ' ' << m_rotation.z();
	}
	void scale( const Vector3& scaling ) override {
		if ( !nothingSelected() ) {
			m_scale = scaling;
			m_repeatableTransforms.setScale( scaling );

			if ( Mode() == eComponent ) {
				Scene_Scale_Component_Selected( GlobalSceneGraph(), m_scale, m_pivot2world.t().vec3() );
			}
			else
			{
				Scene_Scale_Selected( GlobalSceneGraph(), m_scale, m_pivot2world.t().vec3() );
			}

			if( ManipulatorMode() == eSkew ){
				m_pivot2world[0] = scaling[0];
				m_pivot2world[5] = scaling[1];
				m_pivot2world[10] = scaling[2];
			}

			SceneChangeNotify();
		}
	}
	void outputScale( TextOutputStream& ostream ){
		ostream << " -scale " << m_scale.x() << ' ' << m_scale.y() << ' ' << m_scale.z();
	}

	void skew( const Skew& skew ) override {
		if ( !nothingSelected() ) {
			m_skew = skew;
			m_repeatableTransforms.setSkew( skew );

			if ( Mode() == eComponent ) {
				Scene_Skew_Component_Selected( GlobalSceneGraph(), m_skew, m_pivot2world.t().vec3() );
			}
			else
			{
				Scene_Skew_Selected( GlobalSceneGraph(), m_skew, m_pivot2world.t().vec3() );
			}
			m_pivot2world[skew.index] = skew.amount;
			SceneChangeNotify();
		}
	}

	void alltransform( const Transforms& transforms, const Vector3& world_pivot ) override {
		if ( !nothingSelected() ) {
			if ( Mode() == eComponent ) {
				GlobalSelectionSystem().foreachSelectedComponent( transform_component_selected( transforms, world_pivot ) );
			}
			else
			{
				GlobalSelectionSystem().foreachSelected( transform_selected( transforms, world_pivot ) );
			}
			SceneChangeNotify();
		}
	}

	void rotateSelected( const Quaternion& rotation, bool snapOrigin = false ) override {
		if( snapOrigin && !m_pivotIsCustom )
			vector3_snap( m_pivot2world.t().vec3(), GetSnapGridSize() );
		startMove();
		rotate( rotation );
		freezeTransforms();
	}
	void translateSelected( const Vector3& translation ) override {
		startMove();
		translate( translation );
		freezeTransforms();
	}
	void scaleSelected( const Vector3& scaling, bool snapOrigin = false ) override {
		if( snapOrigin && !m_pivotIsCustom )
			vector3_snap( m_pivot2world.t().vec3(), GetSnapGridSize() );
		startMove();
		scale( scaling );
		freezeTransforms();
	}

	TransformsObserved m_repeatableTransforms;

	void repeatTransforms() override {
		extern void Scene_Clone_Selected();
		if ( !nothingSelected() && !m_repeatableTransforms.isIdentity() ) {
			startMove();
			UndoableCommand undo( "repeatTransforms" );
			if( Mode() == ePrimitive )
				Scene_Clone_Selected();
			alltransform( m_repeatableTransforms, m_pivot2world.t().vec3() );
			freezeTransforms();
		}
	}
	void resetTransforms( EManipulatorMode which ) override {
		const bool all = ( which != eTranslate && which != eRotate && which != eScale && which != eSkew );
		if( which == eTranslate || all )
			m_repeatableTransforms.setTranslation( c_translation_identity );
		if( which == eRotate || all )
			m_repeatableTransforms.setRotation( c_rotation_identity );
		if( which == eScale || all )
			m_repeatableTransforms.setScale( c_scale_identity );
		if( which == eSkew || all )
			m_repeatableTransforms.setSkew( c_skew_identity );
	}

	bool transformOrigin_isTranslatable() const {
		return ManipulatorMode() == eScale
		    || ManipulatorMode() == eSkew
		    || ManipulatorMode() == eRotate
		    || ManipulatorMode() == eTranslate;
	}

	void transformOriginTranslate( const Vector3& translation, const bool set[3] ) override {
		m_pivot2world = m_pivot2world_start;
		setCustomTransformOrigin( translation + m_pivot2world_start.t().vec3(), set );
		SceneChangeNotify();
	}

	void MoveSelected( const View& view, const DeviceVector device_point ){
		if ( m_manipulator->isSelected() ) {
			if ( !m_undo_begun ) {
				m_undo_begun = true;
				GlobalUndoSystem().start();
			}

			Matrix4 device2manip;
			ConstructDevice2Manip( device2manip, m_pivot2world_start, view.GetModelview(), view.GetProjection(), view.GetViewport() );
			m_manipulator->GetManipulatable()->Transform( m_manip2pivot_start, device2manip, device_point );
		}
		else if( m_transformOrigin_manipulator.isSelected() ){
			Matrix4 device2manip;
			ConstructDevice2Manip( device2manip, m_pivot2world_start, view.GetModelview(), view.GetProjection(), view.GetViewport() );
			m_transformOrigin_manipulator.GetManipulatable()->Transform( m_manip2pivot_start, device2manip, device_point );
		}
	}

	/// \todo Support view-dependent nudge.
	void NudgeManipulator( const Vector3& nudge, const Vector3& view ) override {
	//	if ( ManipulatorMode() == eTranslate || ManipulatorMode() == eDrag ) {
		translateSelected( nudge );
	//	}
	}

	bool endMove();
	void freezeTransforms();

	void renderSolid( Renderer& renderer, const VolumeTest& volume ) const override;
	void renderWireframe( Renderer& renderer, const VolumeTest& volume ) const override {
		renderSolid( renderer, volume );
	}

	const Matrix4& GetPivot2World() const {
		ConstructPivot();
		return m_pivot2world;
	}

	static void constructStatic(){
#if defined( DEBUG_SELECTION )
		g_state_clipped = GlobalShaderCache().capture( "$DEBUG_CLIPPED" );
#endif
		m_state = GlobalShaderCache().capture( "$POINT" );
		TranslateManipulator::m_state_wire =
		RotateManipulator::m_state_outer =
		SkewManipulator::m_state_wire =
		BuildManipulator::m_state_line = GlobalShaderCache().capture( "$WIRE_OVERLAY" );
		TranslateManipulator::m_state_fill =
		SkewManipulator::m_state_fill = GlobalShaderCache().capture( "$FLATSHADE_OVERLAY" );
		TransformOriginManipulator::m_state =
		ClipManipulator::m_state =
		SkewManipulator::m_state_point =
		BuildManipulator::m_state_point =
		UVManipulator::m_state_point = GlobalShaderCache().capture( "$BIGPOINT" );
		RenderablePivot::StaticShader::instance() = GlobalShaderCache().capture( "$PIVOT" );
		UVManipulator::m_state_line = GlobalShaderCache().capture( "$BLENDLINE" );
		DragManipulator::m_state_wire = GlobalShaderCache().capture( "$PLANE_WIRE_OVERLAY" );
	}

	static void destroyStatic(){
#if defined( DEBUG_SELECTION )
		GlobalShaderCache().release( "$DEBUG_CLIPPED" );
#endif
		GlobalShaderCache().release( "$PLANE_WIRE_OVERLAY" );
		GlobalShaderCache().release( "$BLENDLINE" );
		GlobalShaderCache().release( "$PIVOT" );
		GlobalShaderCache().release( "$BIGPOINT" );
		GlobalShaderCache().release( "$FLATSHADE_OVERLAY" );
		GlobalShaderCache().release( "$WIRE_OVERLAY" );
		GlobalShaderCache().release( "$POINT" );
	}
};

Shader* RadiantSelectionSystem::m_state = 0;


namespace
{
RadiantSelectionSystem* g_RadiantSelectionSystem;

inline RadiantSelectionSystem& getSelectionSystem(){
	return *g_RadiantSelectionSystem;
}
}

#include "map.h"

class testselect_entity_visible : public scene::Graph::Walker
{
	Selector& m_selector;
	SelectionTest& m_test;
public:
	testselect_entity_visible( Selector& selector, SelectionTest& test )
		: m_selector( selector ), m_test( test ){
	}
	bool pre( const scene::Path& path, scene::Instance& instance ) const override {
		if( path.top().get_pointer() == Map_GetWorldspawn( g_map ) ||
		    node_is_group( path.top().get() ) ){
			return false;
		}
		Selectable* selectable = Instance_getSelectable( instance );
		if ( selectable != 0
		     && Node_isEntity( path.top() ) ) {
			m_selector.pushSelectable( *selectable );
		}

		SelectionTestable* selectionTestable = Instance_getSelectionTestable( instance );
		if ( selectionTestable ) {
			selectionTestable->testSelect( m_selector, m_test );
		}

		return true;
	}
	void post( const scene::Path& path, scene::Instance& instance ) const override {
		Selectable* selectable = Instance_getSelectable( instance );
		if ( selectable != 0
		     && Node_isEntity( path.top() ) ) {
			m_selector.popSelectable();
		}
	}
};

class testselect_primitive_visible : public scene::Graph::Walker
{
	Selector& m_selector;
	SelectionTest& m_test;
public:
	testselect_primitive_visible( Selector& selector, SelectionTest& test )
		: m_selector( selector ), m_test( test ){
	}
	bool pre( const scene::Path& path, scene::Instance& instance ) const override {
		Selectable* selectable = Instance_getSelectable( instance );
		if ( selectable != 0 ) {
			m_selector.pushSelectable( *selectable );
		}

		SelectionTestable* selectionTestable = Instance_getSelectionTestable( instance );
		if ( selectionTestable ) {
			selectionTestable->testSelect( m_selector, m_test );
		}

		return true;
	}
	void post( const scene::Path& path, scene::Instance& instance ) const override {
		Selectable* selectable = Instance_getSelectable( instance );
		if ( selectable != 0 ) {
			m_selector.popSelectable();
		}
	}
};

class testselect_component_visible : public scene::Graph::Walker
{
	Selector& m_selector;
	SelectionTest& m_test;
	SelectionSystem::EComponentMode m_mode;
public:
	testselect_component_visible( Selector& selector, SelectionTest& test, SelectionSystem::EComponentMode mode )
		: m_selector( selector ), m_test( test ), m_mode( mode ){
	}
	bool pre( const scene::Path& path, scene::Instance& instance ) const override {
		ComponentSelectionTestable* componentSelectionTestable = Instance_getComponentSelectionTestable( instance );
		if ( componentSelectionTestable ) {
			componentSelectionTestable->testSelectComponents( m_selector, m_test, m_mode );
		}

		return true;
	}
};


class testselect_component_visible_selected : public scene::Graph::Walker
{
	Selector& m_selector;
	SelectionTest& m_test;
	SelectionSystem::EComponentMode m_mode;
public:
	testselect_component_visible_selected( Selector& selector, SelectionTest& test, SelectionSystem::EComponentMode mode )
		: m_selector( selector ), m_test( test ), m_mode( mode ){
	}
	bool pre( const scene::Path& path, scene::Instance& instance ) const override {
		if ( Instance_isSelected( instance ) ) {
			ComponentSelectionTestable* componentSelectionTestable = Instance_getComponentSelectionTestable( instance );
			if ( componentSelectionTestable ) {
				componentSelectionTestable->testSelectComponents( m_selector, m_test, m_mode );
			}
		}

		return true;
	}
};

void Scene_TestSelect_Primitive( Selector& selector, SelectionTest& test, const VolumeTest& volume ){
	Scene_forEachVisible( GlobalSceneGraph(), volume, testselect_primitive_visible( selector, test ) );
}

void Scene_TestSelect_Component_Selected( Selector& selector, SelectionTest& test, const VolumeTest& volume, SelectionSystem::EComponentMode componentMode ){
	Scene_forEachVisible( GlobalSceneGraph(), volume, testselect_component_visible_selected( selector, test, componentMode ) );
}

void Scene_TestSelect_Component( Selector& selector, SelectionTest& test, const VolumeTest& volume, SelectionSystem::EComponentMode componentMode ){
	Scene_forEachVisible( GlobalSceneGraph(), volume, testselect_component_visible( selector, test, componentMode ) );
}

void RadiantSelectionSystem::Scene_TestSelect( Selector& selector, SelectionTest& test, const View& view, SelectionSystem::EMode mode, SelectionSystem::EComponentMode componentMode ){
	switch ( mode )
	{
	case eEntity:
		Scene_forEachVisible( GlobalSceneGraph(), view, testselect_entity_visible( selector, test ) );
		break;
	case ePrimitive:
		Scene_TestSelect_Primitive( selector, test, view );
		break;
	case eComponent:
		Scene_TestSelect_Component_Selected( selector, test, view, componentMode );
		break;
	}
}


void Scene_Intersect( const View& view, const Vector2& device_point, const Vector2& device_epsilon, Vector3& intersection ){
	View scissored( view );
	ConstructSelectionTest( scissored, SelectionBoxForPoint( device_point, device_epsilon ) );
	SelectionVolume test( scissored );

	BestPointSelector bestPointSelector;
	Scene_TestSelect_Primitive( bestPointSelector, test, scissored );

	test.BeginMesh( g_matrix4_identity, true );
	if( bestPointSelector.isSelected() ){
		intersection = vector4_projected( matrix4_transformed_vector4( test.getScreen2world(), Vector4( 0, 0, bestPointSelector.best().depth(), 1 ) ) );
	}
	else{
		const Vector3 pnear = vector4_projected( matrix4_transformed_vector4( test.getScreen2world(), Vector4( 0, 0, -1, 1 ) ) );
		const Vector3 pfar = vector4_projected( matrix4_transformed_vector4( test.getScreen2world(), Vector4( 0, 0, 1, 1 ) ) );
		intersection = vector3_normalised( pfar - pnear ) * 256.f + pnear;
	}
}

class FreezeTransforms : public scene::Graph::Walker
{
public:
	bool pre( const scene::Path& path, scene::Instance& instance ) const override {
		TransformNode* transformNode = Node_getTransformNode( path.top() );
		if ( transformNode != 0 ) {
			Transformable* transform = Instance_getTransformable( instance );
			if ( transform != 0 ) {
				transform->freezeTransform();
			}
		}
		return true;
	}
};

void RadiantSelectionSystem::freezeTransforms(){
	GlobalSceneGraph().traverse( FreezeTransforms() );
}


bool RadiantSelectionSystem::endMove(){
	if( m_transformOrigin_manipulator.isSelected() ){
		if( m_pivot2world == m_pivot2world_start ){
			m_pivotIsCustom = !m_pivotIsCustom;
			pivotChanged();
		}
		return true;
	}

	if ( ManipulatorMode() == eUV )
		m_uv_manipulator.freezeTransform();
	else
		freezeTransforms();

//	if ( Mode() == ePrimitive && ManipulatorMode() == eDrag ) {
//		g_bTmpComponentMode = false;
//		Scene_SelectAll_Component( false, g_modifiers == c_modifierAlt? SelectionSystem::eVertex : SelectionSystem::eFace );
//	}
	if( g_bTmpComponentMode ){
		g_bTmpComponentMode = false;
		setSelectedAllComponents( false );
	}

	m_pivot_moving = false;
	pivotChanged();

	SceneChangeNotify();

	if ( m_undo_begun ) {
		StringOutputStream command( 64 );

		if ( ManipulatorMode() == eTranslate ) {
			command << "translateTool";
			outputTranslation( command );
		}
		else if ( ManipulatorMode() == eRotate ) {
			command << "rotateTool";
			outputRotation( command );
		}
		else if ( ManipulatorMode() == eScale ) {
			command << "scaleTool";
			outputScale( command );
		}
		else if ( ManipulatorMode() == eSkew ) {
			command << "transformTool";
//			outputScale( command );
		}
		else if ( ManipulatorMode() == eDrag ) {
			command << "dragTool";
		}
		else if ( ManipulatorMode() == eUV ) {
			command << "UVTool";
		}

		GlobalUndoSystem().finish( command );
	}
	return false;
}

class bounds_selected_withEntityBounds : public scene::Graph::Walker
{
	AABB& m_bounds;
public:
	bounds_selected_withEntityBounds( AABB& bounds )
		: m_bounds( bounds ){
		m_bounds = AABB();
	}
	bool pre( const scene::Path& path, scene::Instance& instance ) const override {
		const auto getBounds = [&]() -> AABB {
			if( Entity* entity = Node_getEntity( path.top() ) ){
				if ( const EntityClass& eclass = entity->getEntityClass(); eclass.fixedsize && !eclass.miscmodel_is ) {
					Editable* editable = Node_getEditable( path.top() );
					const Vector3 origin = editable != 0
						? matrix4_multiplied_by_matrix4( instance.localToWorld(), editable->getLocalPivot() ).t().vec3()
						: instance.localToWorld().t().vec3();
					return aabb_for_minmax( eclass.mins + origin, eclass.maxs + origin );
				}
			}
			return instance.worldAABB();
		};
		if ( Instance_isSelected( instance ) ) {
			aabb_extend_by_aabb_safe( m_bounds, getBounds() );
		}
		return true;
	}
};

inline AABB Instance_getPivotBounds( scene::Instance& instance ){
	Entity* entity = Node_getEntity( instance.path().top() );
	if ( entity != 0 && !entity->getEntityClass().miscmodel_is
	     && ( entity->getEntityClass().fixedsize
	          || !node_is_group( instance.path().top() ) ) ) {
		Editable* editable = Node_getEditable( instance.path().top() );
		if ( editable != 0 ) {
			return AABB( matrix4_multiplied_by_matrix4( instance.localToWorld(), editable->getLocalPivot() ).t().vec3(), Vector3( 0, 0, 0 ) );
		}
		else
		{
			return AABB( instance.localToWorld().t().vec3(), Vector3( 0, 0, 0 ) );
		}
	}

	return instance.worldAABB();
}

class bounds_selected : public scene::Graph::Walker
{
	AABB& m_bounds;
public:
	bounds_selected( AABB& bounds )
		: m_bounds( bounds ){
		m_bounds = AABB();
	}
	bool pre( const scene::Path& path, scene::Instance& instance ) const override {
		if ( Instance_isSelected( instance ) ) {
			aabb_extend_by_aabb_safe( m_bounds, Instance_getPivotBounds( instance ) );
		}
		return true;
	}
};

class bounds_selected_component : public scene::Graph::Walker
{
	AABB& m_bounds;
public:
	bounds_selected_component( AABB& bounds )
		: m_bounds( bounds ){
		m_bounds = AABB();
	}
	bool pre( const scene::Path& path, scene::Instance& instance ) const override {
		if ( Instance_isSelected( instance ) ) {
			ComponentEditable* componentEditable = Instance_getComponentEditable( instance );
			if ( componentEditable ) {
				aabb_extend_by_aabb_safe( m_bounds, aabb_for_oriented_aabb_safe( componentEditable->getSelectedComponentsBounds(), instance.localToWorld() ) );
			}
		}
		return true;
	}
};

void Scene_BoundsSelected_withEntityBounds( scene::Graph& graph, AABB& bounds ){
	graph.traverse( bounds_selected_withEntityBounds( bounds ) );
}

void Scene_BoundsSelected( scene::Graph& graph, AABB& bounds ){
	graph.traverse( bounds_selected( bounds ) );
}

void Scene_BoundsSelectedComponent( scene::Graph& graph, AABB& bounds ){
	graph.traverse( bounds_selected_component( bounds ) );
}

#if 0
inline void pivot_for_node( Matrix4& pivot, scene::Node& node, scene::Instance& instance ){
	ComponentEditable* componentEditable = Instance_getComponentEditable( instance );
	if ( GlobalSelectionSystem().Mode() == SelectionSystem::eComponent
	     && componentEditable != 0 ) {
		pivot = matrix4_translation_for_vec3( componentEditable->getSelectedComponentsBounds().origin );
	}
	else
	{
		Bounded* bounded = Instance_getBounded( instance );
		if ( bounded != 0 ) {
			pivot = matrix4_translation_for_vec3( bounded->localAABB().origin );
		}
		else
		{
			pivot = g_matrix4_identity;
		}
	}
}
#endif

void RadiantSelectionSystem::ConstructPivotRotation() const {
	switch ( m_manipulator_mode )
	{
	case eTranslate:
		break;
	case eRotate:
		if ( Mode() == eComponent ) {
			matrix4_assign_rotation_for_pivot( m_pivot2world, m_component_selection.back() );
		}
		else
		{
			matrix4_assign_rotation_for_pivot( m_pivot2world, m_selection.back() );
		}
		break;
	case eScale:
		if ( Mode() == eComponent ) {
			matrix4_assign_rotation_for_pivot( m_pivot2world, m_component_selection.back() );
		}
		else
		{
			matrix4_assign_rotation_for_pivot( m_pivot2world, m_selection.back() );
		}
		break;
	default:
		break;
	}
}

void RadiantSelectionSystem::ConstructPivot() const {
	if ( !m_pivotChanged || m_pivot_moving ) {
		return;
	}
	m_pivotChanged = false;

	if ( !nothingSelected() ) {
		m_bounds = getSelectionAABB();
		if( !m_pivotIsCustom ){
			Vector3 object_pivot = m_bounds.origin;

			//vector3_snap( object_pivot, GetSnapGridSize() );
			//globalOutputStream() << object_pivot << '\n';
			m_pivot2world = matrix4_translation_for_vec3( object_pivot );
		}
		else{
//			m_pivot2world = matrix4_translation_for_vec3( m_pivot2world.t().vec3() );
			matrix4_assign_rotation( m_pivot2world, g_matrix4_identity );
		}

		ConstructPivotRotation();
	}
}

void RadiantSelectionSystem::setCustomTransformOrigin( const Vector3& origin, const bool set[3] ) const {
	if ( !nothingSelected() && transformOrigin_isTranslatable() ) {

		//globalOutputStream() << origin << '\n';
		for( std::size_t i = 0; i < 3; i++ ){
			float value = origin[i];
			if( set[i] ){
				float bestsnapDist = fabs( m_bounds.origin[i] - value );
				float bestsnapTo = m_bounds.origin[i];
				float othersnapDist = fabs( m_bounds.origin[i] + m_bounds.extents[i] - value );
				if( othersnapDist < bestsnapDist ){
					bestsnapDist = othersnapDist;
					bestsnapTo = m_bounds.origin[i] + m_bounds.extents[i];
				}
				othersnapDist = fabs( m_bounds.origin[i] - m_bounds.extents[i] - value );
				if( othersnapDist < bestsnapDist ){
					bestsnapDist = othersnapDist;
					bestsnapTo = m_bounds.origin[i] - m_bounds.extents[i];
				}
				othersnapDist = fabs( float_snapped( value, GetSnapGridSize() ) - value );
				if( othersnapDist < bestsnapDist ){
					bestsnapDist = othersnapDist;
					bestsnapTo = float_snapped( value, GetSnapGridSize() );
				}
				value = bestsnapTo;

				m_pivot2world[i + 12] = value; //m_pivot2world.tx() .ty() .tz()
			}
		}
		m_pivotIsCustom = true;

		ConstructPivotRotation();
	}
}

AABB RadiantSelectionSystem::getSelectionAABB() const {
	AABB bounds;
	if ( !nothingSelected() ) {
		if ( Mode() == eComponent || g_bTmpComponentMode ) {
			Scene_BoundsSelectedComponent( GlobalSceneGraph(), bounds );
			if( !aabb_valid( bounds ) ) /* selecting PlaneSelectables sets g_bTmpComponentMode, but only brushes return correct componentEditable->getSelectedComponentsBounds() */
				bounds = getBoundsSelected();
		}
		else
		{
			bounds = getBoundsSelected();
		}
	}
	return bounds;
}

void RadiantSelectionSystem::renderSolid( Renderer& renderer, const VolumeTest& volume ) const {
	//if( view->TestPoint( m_object_pivot ) )
	if ( !nothingSelected()
	     || ManipulatorMode() == eClip
	     || ManipulatorMode() == eBuild
	     || ManipulatorMode() == eUV
	     || ManipulatorMode() == eDrag ) {
		renderer.Highlight( Renderer::ePrimitive, false );
		renderer.Highlight( Renderer::eFace, false );

		renderer.SetState( m_state, Renderer::eWireframeOnly );
		renderer.SetState( m_state, Renderer::eFullMaterials );

		if( transformOrigin_isTranslatable() )
			m_transformOrigin_manipulator.render( renderer, volume, GetPivot2World() );

		m_manipulator->render( renderer, volume, GetPivot2World() );
	}

#if defined( DEBUG_SELECTION )
	renderer.SetState( g_state_clipped, Renderer::eWireframeOnly );
	renderer.SetState( g_state_clipped, Renderer::eFullMaterials );
	renderer.addRenderable( g_render_clipped, g_render_clipped.m_world );
#endif
}

#include "preferencesystem.h"
#include "preferences.h"

void SelectionSystem_constructPreferences( PreferencesPage& page ){
	page.appendSpinner( "Selector size (pixels)", g_SELECT_EPSILON, 2, 64 );
	page.appendCheckBox( "", "Prefer point entities in 2D", getSelectionSystem().m_bPreferPointEntsIn2D );
	page.appendCheckBox( "", "Create brushes in 3D", g_3DCreateBrushes );
	{
		const char* styles[] = { "XY plane + Z with Alt", "View plane + Forward with Alt", };
		page.appendCombo(
		    "Move style in 3D",
		    StringArrayRange( styles ),
		    IntImportCaller( TranslateFreeXY_Z::m_viewdependent ),
		    IntExportCaller( TranslateFreeXY_Z::m_viewdependent )
		);
	}
}
void SelectionSystem_constructPage( PreferenceGroup& group ){
	PreferencesPage page( group.createPage( "Selection", "Selection System Settings" ) );
	SelectionSystem_constructPreferences( page );
}
void SelectionSystem_registerPreferencesPage(){
	PreferencesDialog_addSettingsPage( makeCallbackF( SelectionSystem_constructPage ) );
}


void SelectionSystem_connectTransformsCallbacks( const std::array<Callback<void(const char*)>, 4>& callbacks ){
	getSelectionSystem().m_repeatableTransforms.m_changedCallbacks = callbacks;
}


void SelectionSystem_OnBoundsChanged(){
	getSelectionSystem().pivotChanged();
}

SignalHandlerId SelectionSystem_boundsChanged;

void SelectionSystem_Construct(){
	RadiantSelectionSystem::constructStatic();

	g_RadiantSelectionSystem = new RadiantSelectionSystem;

	SelectionSystem_boundsChanged = GlobalSceneGraph().addBoundsChangedCallback( FreeCaller<void(), SelectionSystem_OnBoundsChanged>() );

	GlobalShaderCache().attachRenderable( getSelectionSystem() );

	GlobalPreferenceSystem().registerPreference( "SELECT_EPSILON", IntImportStringCaller( g_SELECT_EPSILON ), IntExportStringCaller( g_SELECT_EPSILON ) );
	GlobalPreferenceSystem().registerPreference( "PreferPointEntsIn2D", BoolImportStringCaller( getSelectionSystem().m_bPreferPointEntsIn2D ), BoolExportStringCaller( getSelectionSystem().m_bPreferPointEntsIn2D ) );
	GlobalPreferenceSystem().registerPreference( "3DCreateBrushes", BoolImportStringCaller( g_3DCreateBrushes ), BoolExportStringCaller( g_3DCreateBrushes ) );
	GlobalPreferenceSystem().registerPreference( "3DMoveStyle", IntImportStringCaller( TranslateFreeXY_Z::m_viewdependent ), IntExportStringCaller( TranslateFreeXY_Z::m_viewdependent ) );
	SelectionSystem_registerPreferencesPage();
}

void SelectionSystem_Destroy(){
	GlobalShaderCache().detachRenderable( getSelectionSystem() );

	GlobalSceneGraph().removeBoundsChangedCallback( SelectionSystem_boundsChanged );

	delete g_RadiantSelectionSystem;

	RadiantSelectionSystem::destroyStatic();
}




inline float screen_normalised( float pos, std::size_t size ){
	return ( ( 2.0f * pos ) / size ) - 1.0f;
}

inline DeviceVector window_to_normalised_device( WindowVector window, std::size_t width, std::size_t height ){
	return DeviceVector( screen_normalised( window.x(), width ), screen_normalised( height - 1 - window.y(), height ) );
}

inline float device_constrained( float pos ){
	return std::clamp( pos, -1.0f, 1.0f );
}

inline DeviceVector device_constrained( DeviceVector device ){
	return DeviceVector( device_constrained( device.x() ), device_constrained( device.y() ) );
}

inline float window_constrained( float pos, std::size_t origin, std::size_t size ){
	return std::clamp( pos, static_cast<float>( origin ), static_cast<float>( origin + size ) );
}

inline WindowVector window_constrained( WindowVector window, std::size_t x, std::size_t y, std::size_t width, std::size_t height ){
	return WindowVector( window_constrained( window.x(), x, width ), window_constrained( window.y(), y, height ) );
}

typedef Callback<void(DeviceVector)> MouseEventCallback;

Single<MouseEventCallback> g_mouseMovedCallback;
Single<MouseEventCallback> g_mouseUpCallback;

#if 1
const ButtonIdentifier c_button_select = c_buttonLeft;
const ButtonIdentifier c_button_select2 = c_buttonRight;
const ModifierFlags c_modifier_manipulator = c_modifierNone;
const ModifierFlags c_modifier_toggle = c_modifierShift;
const ModifierFlags c_modifier_replace = c_modifierShift | c_modifierAlt;
const ModifierFlags c_modifier_face = c_modifierControl;
#else
const ButtonIdentifier c_button_select = c_buttonLeft;
const ModifierFlags c_modifier_manipulator = c_modifierNone;
const ModifierFlags c_modifier_toggle = c_modifierControl;
const ModifierFlags c_modifier_replace = c_modifierNone;
const ModifierFlags c_modifier_face = c_modifierShift;
#endif
const ModifierFlags c_modifier_toggle_face = c_modifier_toggle | c_modifier_face;
const ModifierFlags c_modifier_replace_face = c_modifier_replace | c_modifier_face;

const ButtonIdentifier c_button_texture = c_buttonMiddle;
const ModifierFlags c_modifier_apply_texture1_project = c_modifierControl | c_modifierShift;
const ModifierFlags c_modifier_apply_texture2_seamless = c_modifierControl;
const ModifierFlags c_modifier_apply_texture3 =                     c_modifierShift;
const ModifierFlags c_modifier_copy_texture = c_modifierNone;



void Scene_copyClosestTexture( SelectionTest& test );
void Scene_applyClosestTexture( SelectionTest& test, bool shift, bool ctrl, bool alt, bool texturize_selected = false );
const char* Scene_applyClosestTexture_getUndoName( bool shift, bool ctrl, bool alt );

class TexManipulator_
{
	const DeviceVector& m_epsilon;
public:
	const View* m_view;
	bool m_undo_begun;

	TexManipulator_( const DeviceVector& epsilon ) :
		m_epsilon( epsilon ),
		m_undo_begun( false ){
	}

	void mouseDown( DeviceVector position ){
		View scissored( *m_view );
		ConstructSelectionTest( scissored, SelectionBoxForPoint( position, m_epsilon ) );
		SelectionVolume volume( scissored );

		if( g_modifiers == c_modifier_copy_texture ) {
			Scene_copyClosestTexture( volume );
		}
		else{
			m_undo_begun = true;
			GlobalUndoSystem().start();
			Scene_applyClosestTexture( volume, g_modifiers.shift(), g_modifiers.ctrl(), g_modifiers.alt(), true );
		}
	}

	void mouseMoved( DeviceVector position ){
		if( m_undo_begun ){
			View scissored( *m_view );
			ConstructSelectionTest( scissored, SelectionBoxForPoint( device_constrained( position ), m_epsilon ) );
			SelectionVolume volume( scissored );

			Scene_applyClosestTexture( volume, g_modifiers.shift(), g_modifiers.ctrl(), g_modifiers.alt() );
		}
	}
	typedef MemberCaller<TexManipulator_, void(DeviceVector), &TexManipulator_::mouseMoved> MouseMovedCaller;

	void mouseUp( DeviceVector position ){
		if( m_undo_begun ){
			GlobalUndoSystem().finish( Scene_applyClosestTexture_getUndoName( g_modifiers.shift(), g_modifiers.ctrl(), g_modifiers.alt() ) );
			m_undo_begun = false;
		}
	}
	typedef MemberCaller<TexManipulator_, void(DeviceVector), &TexManipulator_::mouseUp> MouseUpCaller;
};


class Selector_
{
	bool m1selecting() const {
		return !m_mouse2 && ( g_modifiers == c_modifier_toggle || g_modifiers == c_modifier_face
		|| ( g_modifiers == c_modifierAlt && getSelectionSystem().Mode() == SelectionSystem::eComponent ) ); // select primitives in component mode
	}
	bool m2selecting() const {
		return m_mouse2 && ( g_modifiers == c_modifier_toggle || g_modifiers == c_modifier_face );
	}

	RadiantSelectionSystem::EModifier modifier_for_mouseMoved() const {
		return m_mouseMoved
		       ? RadiantSelectionSystem::eReplace
	           : RadiantSelectionSystem::eCycle;
	}
	RadiantSelectionSystem::EModifier modifier_for_state() const {
		return m2selecting()
		       ? modifier_for_mouseMoved()
		       : RadiantSelectionSystem::eManipulator;
	}

	rect_t getDeviceArea() const {
		const DeviceVector delta( m_current - m_start );
		if ( m_mouseMovedWhilePressed && m2selecting() && delta.x() != 0 && delta.y() != 0 )
			return SelectionBoxForArea( m_start, delta );
		else
			return rect_t();
	}

	void draw_area(){
		m_window_update( getDeviceArea() );
	}

	void m2testSelect( DeviceVector position ){
		const RadiantSelectionSystem::EModifier modifier = modifier_for_state();
		if ( modifier != RadiantSelectionSystem::eManipulator ) {
			const DeviceVector delta( position - m_start );
			if ( m_mouseMovedWhilePressed ) {
				if( delta.x() != 0 && delta.y() != 0 )
					getSelectionSystem().SelectArea( *m_view, SelectionBoxForArea( m_start, delta ), g_modifiers == c_modifier_face );
			}
			else{
				getSelectionSystem().SelectPoint( *m_view, position, m_epsilon, modifier, g_modifiers == c_modifier_face );
			}
		}

		m_start = m_current = DeviceVector( 0.f, 0.f );
		draw_area();
	}

	const DeviceVector& m_epsilon;
public:
	DeviceVector m_start;
	DeviceVector m_current;
	bool m_mouse2;
	bool m_mouseMoved;
	bool m_mouseMovedWhilePressed;
	RadiantSelectionSystem::EModifier m_paintMode;
	const View* m_view;
	RectangleCallback m_window_update;

	Selector_( const DeviceVector& epsilon ) :
		m_epsilon( epsilon ),
		m_start( 0.f, 0.f ),
		m_current( 0.f, 0.f ),
		m_mouse2( false ),
		m_mouseMoved( false ),
		m_mouseMovedWhilePressed( false ){
	}

	void testSelect_simpleM1( DeviceVector position ){
		getSelectionSystem().SelectPoint( *m_view, device_constrained( position ), m_epsilon, modifier_for_mouseMoved(), false );
	}

	void mouseDown( DeviceVector position ){
		m_start = m_current = device_constrained( position );
		m_paintMode = RadiantSelectionSystem::eSelect;
		if( m1selecting() ){
			m_paintMode = getSelectionSystem().SelectPoint_InitPaint( *m_view, position, m_epsilon, g_modifiers == c_modifier_face );
		}
	}

	void mouseMoved( DeviceVector position ){
		m_current = device_constrained( position );
		if( m_mouse2 ){
			draw_area();
		}
		else if( m1selecting() ){
			getSelectionSystem().SelectPoint( *m_view, m_current, m_epsilon, m_paintMode, g_modifiers == c_modifier_face );
		}
	}
	typedef MemberCaller<Selector_, void(DeviceVector), &Selector_::mouseMoved> MouseMovedCaller;

	void mouseUp( DeviceVector position ){
		if( m_mouse2 ){
			m2testSelect( device_constrained( position ) );
		}
		else{
			m_start = m_current = DeviceVector( 0.0f, 0.0f );
		}
	}
	typedef MemberCaller<Selector_, void(DeviceVector), &Selector_::mouseUp> MouseUpCaller;
};


class Manipulator_
{
	DeviceVector getEpsilon() const {
		switch ( getSelectionSystem().ManipulatorMode() )
		{
		case SelectionSystem::eClip:
			return m_epsilon / g_SELECT_EPSILON * ( g_SELECT_EPSILON + 4 );
		case SelectionSystem::eDrag:
		case SelectionSystem::eUV:
			return m_epsilon;
		default: //getSelectionSystem().transformOrigin_isTranslatable()
			return m_epsilon / g_SELECT_EPSILON * 8;
		}
	}
	const DeviceVector& m_epsilon;

public:
	const View* m_view;

	bool m_moving_transformOrigin;
	bool m_mouseMovedWhilePressed;

	Manipulator_( const DeviceVector& epsilon ) :
		m_epsilon( epsilon ),
		m_moving_transformOrigin( false ),
		m_mouseMovedWhilePressed( false ) {
	}

	bool mouseDown( DeviceVector position ){
		if( getSelectionSystem().ManipulatorMode() == SelectionSystem::eClip )
			Clipper_tryDoubleclick(); // this b4 SelectManipulator() to track that latest click added no points (hence 2x click one point)
		return getSelectionSystem().SelectManipulator( *m_view, position, getEpsilon() );
	}

	void mouseMoved( DeviceVector position ){
		if( m_mouseMovedWhilePressed )
			getSelectionSystem().MoveSelected( *m_view, position );
	}
	typedef MemberCaller<Manipulator_, void(DeviceVector), &Manipulator_::mouseMoved> MouseMovedCaller;

	void mouseUp( DeviceVector position ){
		m_moving_transformOrigin = getSelectionSystem().endMove();
	}
	typedef MemberCaller<Manipulator_, void(DeviceVector), &Manipulator_::mouseUp> MouseUpCaller;

	void highlight( DeviceVector position ){
		getSelectionSystem().HighlightManipulator( *m_view, position, getEpsilon() );
	}
};




class RadiantWindowObserver final : public SelectionSystemWindowObserver
{
	DeviceVector m_epsilon;

	int m_width;
	int m_height;

	bool m_mouse_down;

	const float m_moveEpsilon;
	float m_move; /* released move after m_moveEnd, for tunnel selector decision: eReplace or eCycle */
	float m_movePressed; /* pressed move after m_moveStart, for decision: m1 tunnel selector or manipulate and if to do tunnel selector at all */
	DeviceVector m_moveStart;
	DeviceVector m_moveEnd;
	// track latest onMouseMotion() interaction to trigger update onModifierDown(), onModifierUp()
	inline static RadiantWindowObserver *m_latestObserver = nullptr;
	inline static WindowVector m_latestPosition;

	Selector_ m_selector;
	Manipulator_ m_manipulator;
	TexManipulator_ m_texmanipulator;
public:

	RadiantWindowObserver() :
		m_mouse_down( false ),
		m_moveEpsilon( .01f ),
		m_selector( m_epsilon ),
		m_manipulator( m_epsilon ),
		m_texmanipulator( m_epsilon ){
	}
	~RadiantWindowObserver(){
		m_latestObserver = nullptr;
	}
	void release() override {
		delete this;
	}
	void setView( const View& view ) override {
		m_selector.m_view = &view;
		m_manipulator.m_view = &view;
		m_texmanipulator.m_view = &view;
	}
	void setRectangleDrawCallback( const RectangleCallback& callback ) override {
		m_selector.m_window_update = callback;
	}
	void updateEpsilon(){
		m_epsilon = DeviceVector( g_SELECT_EPSILON / static_cast<float>( m_width ), g_SELECT_EPSILON / static_cast<float>( m_height ) );
	}
	void onSizeChanged( int width, int height ) override {
		m_width = width;
		m_height = height;
		updateEpsilon();
	}
	void onMouseDown( const WindowVector& position, ButtonIdentifier button, ModifierFlags modifiers ) override {
		updateEpsilon(); /* could have changed, as it is user setting */

		if( m_mouse_down ) return; /* prevent simultaneous mouse presses */

		const DeviceVector devicePosition( device( position ) );

		if ( button == c_button_select || ( button == c_button_select2 && modifiers != c_modifierNone ) ) {
			m_mouse_down = true;

			const bool clipper2d( button == c_button_select && ClipManipulator::quickCondition( modifiers, *m_manipulator.m_view ) );
			if( clipper2d && getSelectionSystem().ManipulatorMode() != SelectionSystem::eClip )
				ClipperModeQuick();

			if ( button == c_button_select && m_manipulator.mouseDown( devicePosition ) ) {
				g_mouseMovedCallback.insert( MouseEventCallback( Manipulator_::MouseMovedCaller( m_manipulator ) ) );
				g_mouseUpCallback.insert( MouseEventCallback( Manipulator_::MouseUpCaller( m_manipulator ) ) );
			}
			else
			{
				m_selector.m_mouse2 = ( button == c_button_select2 );
				m_selector.mouseDown( devicePosition );
				g_mouseMovedCallback.insert( MouseEventCallback( Selector_::MouseMovedCaller( m_selector ) ) );
				g_mouseUpCallback.insert( MouseEventCallback( Selector_::MouseUpCaller( m_selector ) ) );
			}
		}
		else if ( button == c_button_texture ) {
			m_mouse_down = true;
			m_texmanipulator.mouseDown( devicePosition );
			g_mouseMovedCallback.insert( MouseEventCallback( TexManipulator_::MouseMovedCaller( m_texmanipulator ) ) );
			g_mouseUpCallback.insert( MouseEventCallback( TexManipulator_::MouseUpCaller( m_texmanipulator ) ) );
		}

		m_moveStart = devicePosition;
		m_movePressed = 0.f;
	}
	void onMouseMotion( const WindowVector& position, ModifierFlags modifiers ) override {
		m_selector.m_mouseMoved = mouse_moved_epsilon( position, m_moveEnd, m_move );
		if ( m_mouse_down && !g_mouseMovedCallback.empty() ) {
			m_manipulator.m_mouseMovedWhilePressed = m_selector.m_mouseMovedWhilePressed = mouse_moved_epsilon( position, m_moveStart, m_movePressed );
			g_mouseMovedCallback.get() ( device( position ) );
		}
		else{
			m_manipulator.highlight( device( position ) );
		}
		m_latestObserver = this;
		m_latestPosition = position;
	}
	void onMouseUp( const WindowVector& position, ButtonIdentifier button, ModifierFlags modifiers ) override {
		if ( button != c_buttonInvalid && !g_mouseUpCallback.empty() ) {
			g_mouseUpCallback.get() ( device( position ) );
			g_mouseMovedCallback.clear();
			g_mouseUpCallback.clear();
		}
		if( button == c_button_select	/* L button w/o mouse moved = tunnel selection */
		 && modifiers == c_modifierNone
		 && !m_selector.m_mouseMovedWhilePressed
		 && !m_manipulator.m_moving_transformOrigin
		 && !( getSelectionSystem().Mode() == SelectionSystem::eComponent && getSelectionSystem().ManipulatorMode() == SelectionSystem::eDrag )
		 && getSelectionSystem().ManipulatorMode() != SelectionSystem::eClip
		 && getSelectionSystem().ManipulatorMode() != SelectionSystem::eBuild ){
			m_selector.testSelect_simpleM1( device( position ) );
		}
		if( getSelectionSystem().ManipulatorMode() == SelectionSystem::eClip
		&& button == c_button_select && ( modifiers == c_modifierNone || ClipManipulator::quickCondition( modifiers, *m_manipulator.m_view ) ) )
			Clipper_tryDoubleclickedCut();

		m_mouse_down = false; /* unconditionally drop the flag to surely not lock the onMouseDown() */
		m_manipulator.m_moving_transformOrigin = false;
		m_selector.m_mouseMoved = false;
		m_selector.m_mouseMovedWhilePressed = false;
		m_manipulator.m_mouseMovedWhilePressed = false;
		m_moveEnd = device( position );
		m_move = 0.f;
	}
	void onModifierDown( ModifierFlags type ) override {
		g_modifiers = bitfield_enable( g_modifiers, type );
		if( this == m_latestObserver )
			onMouseMotion( m_latestPosition, g_modifiers );
	}
	void onModifierUp( ModifierFlags type ) override {
		g_modifiers = bitfield_disable( g_modifiers, type );
		if( this == m_latestObserver )
			onMouseMotion( m_latestPosition, g_modifiers );
	}
	DeviceVector device( WindowVector window ) const {
		return window_to_normalised_device( window, m_width, m_height );
	}
	bool mouse_moved_epsilon( const WindowVector& position, const DeviceVector& moveStart, float& move ){
		if( move > m_moveEpsilon )
			return true;
		const DeviceVector devicePosition( device( position ) );
		const float currentMove = std::max( fabs( devicePosition.x() - moveStart.x() ), fabs( devicePosition.y() - moveStart.y() ) );
		move = std::max( move, currentMove );
	//	globalOutputStream() << move << " move\n";
		return move > m_moveEpsilon;
	}
	/* support mouse_moved_epsilon with frozen pointer (camera freelook) */
	void incMouseMove( const WindowVector& delta ) override {
		const WindowVector normalized_delta( delta.x() * 2.f / m_width, delta.y() * 2.f / m_height );
		m_moveEnd -= normalized_delta;
		if( m_mouse_down )
			m_moveStart -= normalized_delta;
	}
};



SelectionSystemWindowObserver* NewWindowObserver(){
	return new RadiantWindowObserver;
}



#include "modulesystem/singletonmodule.h"
#include "modulesystem/moduleregistry.h"

class SelectionDependencies :
	public GlobalSceneGraphModuleRef,
	public GlobalShaderCacheModuleRef,
	public GlobalOpenGLModuleRef
{
};

class SelectionAPI : public TypeSystemRef
{
	SelectionSystem* m_selection;
public:
	typedef SelectionSystem Type;
	STRING_CONSTANT( Name, "*" );

	SelectionAPI(){
		SelectionSystem_Construct();

		m_selection = &getSelectionSystem();
	}
	~SelectionAPI(){
		SelectionSystem_Destroy();
	}
	SelectionSystem* getTable(){
		return m_selection;
	}
};

typedef SingletonModule<SelectionAPI, SelectionDependencies> SelectionModule;
typedef Static<SelectionModule> StaticSelectionModule;
StaticRegisterModule staticRegisterSelection( StaticSelectionModule::instance() );
